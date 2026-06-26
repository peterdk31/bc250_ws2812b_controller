#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include "config_loader.hpp"
#include "color_lut.hpp"
#include "protocol.hpp"

// The host-side LED canvas. Effects render into it via setPixel(); it owns
// the color LUT (brightness/gamma/white-balance/dither) and assembles the
// wire frame (header + mapped pixels + checksum, see common/protocol.hpp).
//
// It owns no output. endFrame() stamps the checksum and hands back the
// finished wire bytes; the daemon passes those to whatever Sinks it built
// from config — the serial hardware, the on-screen viewer, or none at all
// (see host/sink.hpp). Splitting the canvas from the transport this way is
// what lets the daemon run with no serial device attached.
class Strip
{
public:
    static Strip fromConfig(const Config& cfg)
    {
        int leds = cfg.getInt("strip.leds", 10);
        int pin = cfg.getInt("strip.pin", 13);
        float brightness = cfg.getFloat("strip.brightness", 0.1f);

        Strip strip;

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

        // dither: "spatial" (default; the diffuser blends adjacent LEDs)
        // or "temporal" (for a bare strip). See color_lut.hpp.
        strip.setDither(cfg.get("strip.dither", "spatial").c_str());

        // flip the logical-to-physical mapping when the strip is wired
        // so LED 0 is at the far end; effects stay direction-agnostic
        strip.setReversed(cfg.getBool("strip.reverse", false));

        return strip;
    }

    Strip()
    {
        leds = 0;
        pin = 0;
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
    void setDither(const char* name) { lut.setDither(name); }

    void beginFrame()
    {
        frame_++; // advances the per-frame dither (see ColorLut)

        if (leds <= 0) return;

        buf[0] = proto::SYNC0;
        buf[1] = proto::SYNC1;
        buf[2] = (uint8_t)pin;

        buf[3] = (uint8_t)(leds & 0xFF);
        buf[4] = (uint8_t)(leds >> 8);

        // anim id and crossfade duration (see protocol.hpp). The id may be
        // re-stamped after the effect renders (a composite reports its active
        // child), so setAnimId() rewrites these bytes in place before endFrame.
        buf[5] = (uint8_t)(animId_ & 0xFF);
        buf[6] = (uint8_t)(animId_ >> 8);
        buf[7] = (uint8_t)(transitionMs_ & 0xFF);
        buf[8] = (uint8_t)(transitionMs_ >> 8);
    }

    // the global crossfade duration stamped on every frame; set once from
    // config. The receiver dissolves over this whenever the anim id changes.
    void setTransitionMs(uint16_t ms) { transitionMs_ = ms; }

    // id of the animation this frame belongs to. Call after render() (so a
    // composite effect's active child is reflected) and before endFrame(); the
    // receiver crossfades when it sees this change frame-to-frame.
    void setAnimId(uint16_t id)
    {
        animId_ = id;

        if (buf.size() >= proto::PIX_HEADER)
        {
            buf[5] = (uint8_t)(id & 0xFF);
            buf[6] = (uint8_t)(id >> 8);
        }
    }

    void setPixel(int i, uint8_t r, uint8_t g, uint8_t b)
    {
        if (i < 0 || i >= leds) return;

        if (reversed) i = leds - 1 - i;

        int p = proto::PIX_HEADER + i * 3;

        buf[p++] = lut.map(0, r, frame_, i);
        buf[p++] = lut.map(1, g, frame_, i);
        buf[p]   = lut.map(2, b, frame_, i);
    }

    // finalize the current frame and hand back the exact wire bytes (header
    // + pixels + trailing checksum). Call after the effect has rendered (and
    // after any host-side compositing); pass the result to each Sink. The
    // checksum is XOR of pin, count and pixel bytes, so the receiver can drop
    // corrupt frames and resync. The reference is valid until the next
    // beginFrame().
    const std::vector<uint8_t>& endFrame()
    {
        if (!buf.empty())
        {
            uint8_t sum = 0;

            for (size_t i = 2; i + 1 < buf.size(); i++)
                sum ^= buf[i];

            buf.back() = sum;
        }

        return buf;
    }

    int size() const { return leds; }

    float getBrightness() const { return lut.brightness(); }

private:
    void resize()
    {
        if (leds <= 0)
        {
            buf.clear();
            return;
        }

        buf.resize(proto::PIX_HEADER + leds * 3 + 1); // header + pixels + checksum
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
    std::vector<uint8_t> buf;

    int leds;
    int pin;
    bool reversed = false;
    ColorLut lut;
    uint32_t frame_ = 0; // per-frame dither phase, bumped in beginFrame()
    uint16_t animId_ = 0;       // stamped per frame; change triggers a crossfade
    uint16_t transitionMs_ = 0; // crossfade duration carried on every frame
};
