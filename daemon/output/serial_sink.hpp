#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

        if (isHeadless(port))
        {
            fprintf(stderr, "serial: headless (port \"%s\"); no hardware output\n",
                    port.c_str());
            return nullptr;
        }

        return std::unique_ptr<SerialSink>(new SerialSink(port.c_str(), baud, debug));
    }

    SerialSink(const char* port, int baud = 921600, bool debug = false)
        : debug_(debug)
    {
        speed_t speed = baudToSpeed(baud);

        if (speed == B0)
        {
            fprintf(stderr, "unsupported baud rate: %d\n", baud);
            exit(1);
        }

        // the debug backchannel needs to read replies, so open read/write;
        // otherwise keep the port write-only as before
        fd = open(port, (debug_ ? O_RDWR : O_WRONLY) | O_NOCTTY);

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
        {
            fprintf(stderr, "serial: debug log backchannel on (%s)\n", port);
            reader_ = std::thread(&SerialSink::readerLoop, this);
        }
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

    // read the return direction and forward each valid LOG frame (protocol.hpp)
    // to stderr -> journald. Runs only when the debug backchannel is on.
    void readerLoop()
    {
        enum { SCAN, TYPE, HDR, DATA, SUM } st = SCAN;
        uint8_t hdr[9];
        int hn = 0, len = 0, have = 0;
        uint32_t seq = 0, ms = 0;
        uint8_t sum = 0;
        char text[256];
        uint8_t buf[256];

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
                    if (b == proto::LOG_SYNC) { st = HDR; hn = 0; }
                    else if (b != proto::SYNC0) st = SCAN;
                    break;
                case HDR:
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
                        st = (len == 0) ? SUM : DATA;
                    }
                    break;
                case DATA:
                    text[have++] = (char)b;
                    sum ^= b;
                    if (have == len) st = SUM;
                    break;
                case SUM:
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
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> lastSeq_{0}; // highest log seq received so far
    std::chrono::steady_clock::time_point lastDrain_{};
};
