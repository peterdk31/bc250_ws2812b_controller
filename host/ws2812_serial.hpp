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
#include "color_lut.hpp"
#include "protocol.hpp"

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

        WS2812Serial strip(port.c_str(), baud);

        strip.setLeds(leds);
        strip.setPin(pin);

        // gamma: one value for all channels, or three space-separated
        // to also trim the per-channel falloff that makes dim whites
        // drift toward a tint (e.g. "2.0 2.2 2.4")
        float gr, gg, gb;
        parseGamma(cfg.get("strip.gamma", "2.2"), gr, gg, gb);
        strip.setGamma(gr, gg, gb);

        // white_balance: the RRGGBB "white" that reads neutral through
        // the strip's diffuser/filter; applied as a per-channel linear
        // gain so it holds at every brightness
        strip.setWhiteBalance(parseColor(cfg.get("strip.white_balance",
                                                  "ffffff")));

        strip.setBrightness(brightness);

        // flip the logical-to-physical mapping when the strip is wired
        // so LED 0 is at the far end; effects stay direction-agnostic
        strip.setReversed(cfg.getBool("strip.reverse", false));

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
    }

    WS2812Serial(const WS2812Serial&) = delete;
    WS2812Serial& operator=(const WS2812Serial&) = delete;

    WS2812Serial(WS2812Serial&& other) noexcept
        : fd(other.fd),
          buf(std::move(other.buf)),
          leds(other.leds),
          pin(other.pin),
          reversed(other.reversed),
          lut(other.lut),
          frame_(other.frame_)
    {
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

    void setReversed(bool r) { reversed = r; }

    // WS2812 PWM is linear in light output, but effect colors are
    // perceptual; see color_lut.hpp for how these three stack
    void setBrightness(float b) { lut.setBrightness(b); }
    void setGamma(float g) { lut.setGamma(g); }
    void setGamma(float r, float g, float b) { lut.setGamma(r, g, b); }
    void setWhiteBalance(uint32_t rgb) { lut.setWhiteBalance(rgb); }

    void beginFrame()
    {
        frame_++; // advances the per-frame dither (see ColorLut)

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

        if (reversed) i = leds - 1 - i;

        int p = 5 + i * 3;

        buf[p++] = lut.map(0, r, ditherThreshold(frame_, i, 0));
        buf[p++] = lut.map(1, g, ditherThreshold(frame_, i, 1));
        buf[p]   = lut.map(2, b, ditherThreshold(frame_, i, 2));
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

    float getBrightness() const { return lut.brightness(); }

    // host-side frame compositing (crossfading between effects): grab the
    // current frame's already-mapped pixel bytes, or write a blended set
    // back. The header and trailing checksum are left alone, so a
    // snapshot/writePixels pair composites cleanly between beginFrame()
    // and show(). Values are post-LUT, so blending them is a plain linear
    // dissolve in the same space the strip displays.
    std::vector<uint8_t> snapshotPixels() const
    {
        if (leds <= 0) return {};
        return std::vector<uint8_t>(buf.begin() + 5, buf.begin() + 5 + leds * 3);
    }

    void writePixels(const std::vector<uint8_t>& px)
    {
        if (leds <= 0 || (int)px.size() < leds * 3) return;

        for (int i = 0; i < leds * 3; i++)
            buf[5 + i] = px[i];
    }

    // out-of-band command to the receiver (see protocol.hpp): a
    // length-prefixed, checksummed frame the pixel-frame parser skips
    // over. Blocks until the bytes are on the wire (tcdrain) so a caller
    // can send one and exit immediately
    bool sendCommand(uint8_t cmd, const uint8_t* payload = nullptr,
                     uint16_t len = 0)
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

        size_t sent = 0;

        while (sent < frame.size())
        {
            ssize_t n = write(fd, frame.data() + sent, frame.size() - sent);

            if (n < 0)
            {
                if (errno == EINTR) continue;
                perror("serial write");
                return false;
            }

            sent += n;
        }

        tcdrain(fd);
        return true;
    }

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

    // RRGGBB hex, optionally '#'-prefixed, as 0xRRGGBB
    static uint32_t parseColor(std::string v)
    {
        if (!v.empty() && v[0] == '#')
            v.erase(0, 1);

        return (uint32_t)strtoul(v.c_str(), nullptr, 16);
    }

    // one float broadcasts to every channel; three set them per-channel
    static void parseGamma(const std::string& s,
                           float& r, float& g, float& b)
    {
        float v[3];
        int n = sscanf(s.c_str(), "%f %f %f", &v[0], &v[1], &v[2]);

        if (n == 3) { r = v[0]; g = v[1]; b = v[2]; return; }

        r = g = b = (n >= 1) ? v[0] : 2.2f;
    }

private:
    int fd;
    std::vector<uint8_t> buf;

    int leds;
    int pin;
    bool reversed = false;
    ColorLut lut;
    uint32_t frame_ = 0; // per-frame dither phase, bumped in beginFrame()
};
