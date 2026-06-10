#pragma once

#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "config_loader.hpp"

class WS2812Serial
{
public:
    static WS2812Serial fromConfig(const Config& cfg)
    {
        std::string port = cfg.get("serial.port", "/dev/ttyUSB0");
        int baud = cfg.getInt("serial.baud", 921600);
        int leds = cfg.getInt("strip.leds", 10);
        int pin = cfg.getInt("strip.pin", 13);
        float brightness = cfg.getFloat("strip.brightness", 0.1f);
        float gamma = cfg.getFloat("strip.gamma", 2.2f);

        WS2812Serial strip(port.c_str(), baud);

        strip.setLeds(leds);
        strip.setPin(pin);
        strip.setGamma(gamma);
        strip.setBrightness(brightness);

        return strip;
    }

    WS2812Serial(const char* port, int baud = 921600)
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

        leds = 0;
        pin = 0;
        brightness = 1.0f;
        gamma = 2.2f;
        rebuildLut();
    }

    WS2812Serial(const WS2812Serial&) = delete;
    WS2812Serial& operator=(const WS2812Serial&) = delete;

    WS2812Serial(WS2812Serial&& other) noexcept
        : fd(other.fd),
          buf(std::move(other.buf)),
          leds(other.leds),
          pin(other.pin),
          brightness(other.brightness),
          gamma(other.gamma)
    {
        memcpy(lut, other.lut, sizeof lut);
        other.fd = -1;
    }

    ~WS2812Serial()
    {
        if (fd >= 0) close(fd);
    }

    void setLeds(int l)
    {
        leds = l;
        resize();
    }

    void setPin(int p)
    {
        pin = p;
    }

    void setBrightness(float b)
    {
        if (b < 0) b = 0;
        if (b > 1) b = 1;
        brightness = b;
        rebuildLut();
    }

    // WS2812 PWM is linear in light output, but effect colors are
    // perceptual; 1.0 disables correction
    void setGamma(float g)
    {
        gamma = g > 0 ? g : 1.0f;
        rebuildLut();
    }

    void beginFrame()
    {
        if (leds <= 0) return;

        buf[0] = 0xAA;
        buf[1] = 0x55;
        buf[2] = (uint8_t)pin;

        buf[3] = (uint8_t)(leds & 0xFF);
        buf[4] = (uint8_t)(leds >> 8);
    }

    void setPixel(int i, uint8_t r, uint8_t g, uint8_t b)
    {
        if (i < 0 || i >= leds) return;

        int p = 5 + i * 3;

        buf[p++] = lut[r];
        buf[p++] = lut[g];
        buf[p]   = lut[b];
    }

    bool show()
    {
        // trailing checksum: XOR of pin, count and pixel bytes, so the
        // receiver can drop corrupt frames and resync
        if (!buf.empty())
        {
            uint8_t sum = 0;

            for (size_t i = 2; i + 1 < buf.size(); i++)
                sum ^= buf[i];

            buf.back() = sum;
        }

        size_t sent = 0;

        while (sent < buf.size())
        {
            ssize_t n = write(fd, buf.data() + sent, buf.size() - sent);

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

    int size() const { return leds; }

private:
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

    void resize()
    {
        if (leds <= 0)
        {
            buf.clear();
            return;
        }

        buf.resize(5 + leds * 3 + 1); // header + pixels + checksum
    }

    // gamma first, then brightness scales linear light
    void rebuildLut()
    {
        for (int v = 0; v < 256; v++)
            lut[v] = (uint8_t)(powf(v / 255.0f, gamma)
                               * brightness * 255.0f + 0.5f);
    }

private:
    int fd;
    std::vector<uint8_t> buf;

    int leds;
    int pin;
    float brightness;
    float gamma;
    uint8_t lut[256];
};
