#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <string>
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

        if (isHeadless(port))
        {
            fprintf(stderr, "serial: headless (port \"%s\"); no hardware output\n",
                    port.c_str());
            return nullptr;
        }

        return std::unique_ptr<SerialSink>(new SerialSink(port.c_str(), baud));
    }

    SerialSink(const char* port, int baud = 921600)
    {
        speed_t speed = baudToSpeed(baud);

        if (speed == B0)
        {
            fprintf(stderr, "unsupported baud rate: %d\n", baud);
            exit(1);
        }

        fd = open(port, O_WRONLY | O_NOCTTY);

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
    }

    SerialSink(const SerialSink&) = delete;
    SerialSink& operator=(const SerialSink&) = delete;

    ~SerialSink()
    {
        if (fd >= 0) close(fd);
    }

    bool send(const std::vector<uint8_t>& frame) override
    {
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
};
