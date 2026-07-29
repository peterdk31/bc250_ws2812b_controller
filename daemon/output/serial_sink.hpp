#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "config_loader.hpp"
#include "protocol.hpp"
#include "sink.hpp"

// Sink that writes wire frames to the ESP32 over a serial port — the real
// hardware output. The Strip produces the bytes; this just transmits them.
class SerialSink : public Sink
{
public:
    // Build from config, or return nullptr to run headless (no hardware).
    //
    // The port comes from "sinks.serial.port", overridable for a single run by the
    // LED_PORT environment variable so a dev box with no hardware can drive
    // the on-screen viewer without editing config.json:
    //
    //   LED_PORT=none ./led config.json aurora
    //
    // A port of "none"/"null"/"off" (or empty) means headless: no device is
    // opened and this returns nullptr, so the daemon simply runs without a
    // SerialSink in its list. A configured *real* port that won't open is
    // still fatal (exit), so production fails loudly and systemd restarts us
    // to reopen the device rather than silently running blind.
    static std::unique_ptr<SerialSink> fromConfig(const Config& cfg)
    {
        std::string port = cfg.get("sinks.serial.port", "/dev/ttyUSB0");

        if (const char* env = getenv("LED_PORT"))
            port = env;

        int baud = cfg.getInt("sinks.serial.baud", 921600);

        // opt-in debug backchannel: when set, the receiver's in-RAM log
        // (firmware/main/dbglog.*) is drained over the return direction of the
        // link and forwarded to stderr, i.e. journalctl. Off = today's
        // behavior exactly (write-only, no reads, nothing polled).
        bool debug = cfg.getBool("sinks.serial.debug_log", false);

        // opt-in power button: the receiver's power switch (firmware/main/
        // power_switch.cpp) sends a REQ_HOST_SHUTDOWN when its button gets a
        // short press while the machine is up — the ordinary PC power-button
        // gesture, which only the OS can honor. With this set we honor it by
        // running `command`. Deliberately off by default: a byte sequence on a
        // serial port that powers the machine down deserves an explicit yes,
        // and a box without the button wired should never grow the behavior
        // just by updating the daemon.
        bool button = cfg.getBool("sinks.serial.power_button", false);
        std::string buttonCmd = cfg.get("sinks.serial.power_button_command",
                                        "systemctl poweroff");

        if (isHeadless(port))
        {
            fprintf(stderr, "serial: headless (port \"%s\"); no hardware output\n",
                    port.c_str());
            return nullptr;
        }

        return std::unique_ptr<SerialSink>(
            new SerialSink(port.c_str(), baud, debug, button, buttonCmd));
    }

    SerialSink(const char* port, int baud = 921600, bool debug = false,
               bool powerButton = false,
               const std::string& powerCmd = "systemctl poweroff")
        : debug_(debug), powerButton_(powerButton), powerCmd_(powerCmd)
    {
        speed_t speed = baudToSpeed(baud);

        if (speed == B0)
        {
            fprintf(stderr, "unsupported baud rate: %d\n", baud);
            exit(1);
        }

        // both backchannel features need the return direction, so open
        // read/write for them; with neither on, keep the port write-only and
        // nothing is ever read or parsed
        fd = open(port, (reading() ? O_RDWR : O_WRONLY) | O_NOCTTY);

        if (fd < 0)
        {
            perror("serial open");
            exit(1);
        }

        // raw mode at the receiver's baud rate; the port may be left in
        // any state by previous users
        termios tio;

        if (tcgetattr(fd, &tio) != 0)
        {
            perror("tcgetattr");
            exit(1);
        }

        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL;
        tio.c_cflag &= ~CRTSCTS;
        cfsetispeed(&tio, speed);
        cfsetospeed(&tio, speed);

        if (tcsetattr(fd, TCSANOW, &tio) != 0)
        {
            perror("tcsetattr");
            exit(1);
        }

        if (debug_)
            fprintf(stderr, "serial: debug log backchannel on (%s)\n", port);

        if (powerButton_)
            fprintf(stderr, "serial: receiver power button on, will run \"%s\"\n",
                    powerCmd_.c_str());

        if (reading())
            reader_ = std::thread(&SerialSink::readerLoop, this);
    }

    SerialSink(const SerialSink&) = delete;
    SerialSink& operator=(const SerialSink&) = delete;

    ~SerialSink()
    {
        stop_ = true;
        if (reader_.joinable())
            reader_.join();
        if (fd >= 0) close(fd);
    }

    bool send(const std::vector<uint8_t>& frame) override
    {
        // piggyback a log-drain request on the frame stream (~1/s). This is the
        // sole writer thread, so it can't interleave with a pixel frame.
        if (debug_)
            maybeDrain();

        // and, for the same reason, the reader thread hands its acks here rather
        // than writing them itself — two threads writing a tty can interleave
        // mid-frame. Frames go out every few tens of ms, so this is prompt.
        if (uint64_t ack = ackPending_.exchange(0, std::memory_order_relaxed))
        {
            uint8_t p[5];
            p[0] = (uint8_t)(ack >> 32); // req
            for (int i = 0; i < 4; i++)
                p[1 + i] = (uint8_t)(ack >> (8 * i)); // nonce, little-endian

            sendCommand(proto::CMD_REQ_ACK, p, sizeof p);
        }

        if (frame.empty())
            return true;

        // a write failure is fatal: returning false stops the daemon so
        // systemd restarts us and reopens the port
        return writeAll(frame.data(), frame.size());
    }

    // out-of-band command to the receiver (see protocol.hpp): a
    // length-prefixed, checksummed frame the pixel-frame parser skips
    // over. Blocks until the bytes are on the wire (tcdrain) so a caller
    // can send one and exit immediately
    bool sendCommand(uint8_t cmd, const uint8_t* payload = nullptr,
                     uint16_t len = 0) override
    {
        std::vector<uint8_t> frame;
        frame.reserve(6 + len);

        frame.push_back(proto::SYNC0);
        frame.push_back(proto::CMD_SYNC);
        frame.push_back(cmd);
        frame.push_back((uint8_t)(len & 0xFF));
        frame.push_back((uint8_t)(len >> 8));

        uint8_t sum = cmd ^ (uint8_t)(len & 0xFF) ^ (uint8_t)(len >> 8);

        for (uint16_t i = 0; i < len; i++)
        {
            frame.push_back(payload[i]);
            sum ^= payload[i];
        }

        frame.push_back(sum);

        if (!writeAll(frame.data(), frame.size()))
            return false;

        tcdrain(fd);
        return true;
    }

private:
    static bool isHeadless(const std::string& port)
    {
        return port.empty() || port == "none" || port == "null"
            || port == "off";
    }

    // send CMD_LOG_DRAIN at most once a second, asking for everything past the
    // highest seq we've received. No tcdrain (unlike sendCommand): it rides the
    // frame stream and mustn't block it.
    void maybeDrain()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - lastDrain_ < std::chrono::seconds(1))
            return;
        lastDrain_ = now;

        uint32_t s = lastSeq_.load(std::memory_order_relaxed);
        uint8_t f[10];
        f[0] = proto::SYNC0;
        f[1] = proto::CMD_SYNC;
        f[2] = proto::CMD_LOG_DRAIN;
        f[3] = 4; // payload len (little-endian u16)
        f[4] = 0;
        uint8_t sum = f[2] ^ f[3] ^ f[4];
        for (int i = 0; i < 4; i++)
        {
            f[5 + i] = (uint8_t)(s >> (8 * i));
            sum ^= f[5 + i];
        }
        f[9] = sum;

        writeAll(f, sizeof f); // best-effort; a real fault stops us via send()
    }

    bool reading() const { return debug_ || powerButton_; }

    // Act on a request frame from the receiver. Reader thread only.
    void onRequest(uint8_t req, uint32_t nonce)
    {
        if (req != proto::REQ_HOST_SHUTDOWN)
            return; // a newer receiver asking for something we don't know

        if (!powerButton_)
        {
            // we parse the return direction whenever we read at all (i.e. for
            // debug_log alone too), but acting is opt-in. Say so once: from the
            // button's end an ignored press is indistinguishable from a broken
            // wire, and this line is the difference.
            if (!warnedOff_)
            {
                warnedOff_ = true;
                fprintf(stderr, "serial: receiver asked for a graceful shutdown, "
                                "but sinks.serial.power_button is off — ignoring\n");
            }
            return;
        }

        // Ack every request, repeats included: the receiver re-asks until it
        // hears one and reports the silence on its LED. Queue the ack before
        // starting the shutdown, so it still goes out on a frame or two before
        // systemd stops us.
        ackPending_.store(((uint64_t)req << 32) | nonce, std::memory_order_relaxed);

        if (poweringOff_.exchange(true))
            return; // already running; the repeats are just asking again

        fprintf(stderr, "serial: receiver power button — running \"%s\"\n",
                powerCmd_.c_str());
        spawnDetached(powerCmd_);
    }

    // Run a command without waiting on it. Double-forked so the grandchild is
    // reparented to init and can't sit as a zombie in a daemon that is, after
    // all, about to be told to exit; exec'd through sh so the config can hold a
    // plain command line. It inherits our privileges — root, under the shipped
    // unit, which is what makes a bare `systemctl poweroff` work.
    static void spawnDetached(const std::string& cmd)
    {
        pid_t mid = fork();

        if (mid < 0)
        {
            perror("serial: fork");
            return;
        }

        if (mid > 0)
        {
            waitpid(mid, nullptr, 0); // exits immediately, see below
            return;
        }

        if (fork() == 0)
        {
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(127);
        }

        _exit(0);
    }

    // read the return direction and dispatch each valid receiver→host frame
    // (protocol.hpp): LOG frames to stderr -> journald, request frames to
    // onRequest. Runs only when one of the two backchannel features is on.
    void readerLoop()
    {
        enum { SCAN, TYPE, LOG_HDR, LOG_DATA, LOG_SUM, REQ_BODY } st = SCAN;
        uint8_t hdr[9];
        int hn = 0, len = 0, have = 0;
        uint32_t seq = 0, ms = 0;
        uint8_t sum = 0;
        char text[256];
        uint8_t buf[256];
        uint8_t rq[6];
        int rn = 0;

        while (!stop_.load(std::memory_order_relaxed))
        {
            pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 200) <= 0)
                continue; // timeout / signal: re-check stop_

            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0)
            {
                if (n < 0 && (errno == EAGAIN || errno == EINTR))
                    continue;
                break; // EOF or hard error
            }

            for (ssize_t i = 0; i < n; i++)
            {
                uint8_t b = buf[i];
                switch (st)
                {
                case SCAN:
                    if (b == proto::SYNC0) st = TYPE;
                    break;
                case TYPE:
                    if (b == proto::LOG_SYNC) { st = LOG_HDR; hn = 0; }
                    else if (b == proto::REQ_SYNC) { st = REQ_BODY; rn = 0; }
                    else if (b != proto::SYNC0) st = SCAN;
                    break;
                case REQ_BODY:
                    // req(1) nonce(4) checksum
                    rq[rn++] = b;
                    if (rn == 6)
                    {
                        uint8_t s = 0;
                        for (int k = 0; k < 5; k++) s ^= rq[k];

                        if (s == rq[5])
                            onRequest(rq[0],
                                      (uint32_t)rq[1] | ((uint32_t)rq[2] << 8)
                                          | ((uint32_t)rq[3] << 16)
                                          | ((uint32_t)rq[4] << 24));

                        st = SCAN;
                    }
                    break;
                case LOG_HDR:
                    hdr[hn++] = b;
                    if (hn == 9)
                    {
                        seq = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8)
                            | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
                        ms = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8)
                            | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
                        len = hdr[8];
                        sum = 0;
                        for (int k = 0; k < 9; k++) sum ^= hdr[k];
                        have = 0;
                        st = (len == 0) ? LOG_SUM : LOG_DATA;
                    }
                    break;
                case LOG_DATA:
                    text[have++] = (char)b;
                    sum ^= b;
                    if (have == len) st = LOG_SUM;
                    break;
                case LOG_SUM:
                    if (b == sum)
                    {
                        text[len] = 0;
                        fprintf(stderr, "esp %u@%ums: %s\n", seq, ms, text);
                        uint32_t seen = lastSeq_.load(std::memory_order_relaxed);
                        if (seq > seen)
                            lastSeq_.store(seq, std::memory_order_relaxed);
                    }
                    st = SCAN;
                    break;
                }
            }
        }
    }

    bool writeAll(const uint8_t* data, size_t size)
    {
        size_t sent = 0;

        while (sent < size)
        {
            ssize_t n = write(fd, data + sent, size - sent);

            if (n < 0)
            {
                if (errno == EINTR) continue;
                perror("serial write");
                return false;
            }

            sent += n;
        }

        return true;
    }

    static speed_t baudToSpeed(int baud)
    {
        switch (baud)
        {
            case 9600:    return B9600;
            case 19200:   return B19200;
            case 38400:   return B38400;
            case 57600:   return B57600;
            case 115200:  return B115200;
            case 230400:  return B230400;
            case 460800:  return B460800;
            case 500000:  return B500000;
            case 921600:  return B921600;
            case 1000000: return B1000000;
            case 1500000: return B1500000;
            case 2000000: return B2000000;
            default:      return B0;
        }
    }

    int fd = -1;

    // debug backchannel (see fromConfig); all inert when debug_ is false
    bool debug_ = false;
    std::atomic<uint32_t> lastSeq_{0}; // highest log seq received so far
    std::chrono::steady_clock::time_point lastDrain_{};

    // receiver power button (see fromConfig); inert when powerButton_ is false
    bool powerButton_ = false;
    std::string powerCmd_;
    std::atomic<bool> poweringOff_{false}; // the command has been run once
    bool warnedOff_ = false;               // reader thread only

    // the reader thread's return direction, in both senses: it reads frames,
    // and hands acks back to the writer (see send())
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> ackPending_{0}; // (req<<32)|nonce; 0 = none pending
                                          // (the receiver never sends nonce 0)
};
