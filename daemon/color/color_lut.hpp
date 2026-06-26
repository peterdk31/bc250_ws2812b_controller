#pragma once

#include <stdint.h>
#include <math.h>
#include <string.h>

// the per-channel color correction the strip applies to every pixel:
// gamma curves each channel, a white-balance gain rescales it, then a
// global brightness scales the whole thing — all folded into one
// 3x256 lookup table so applying it per pixel is a single array read.
//
// used by the host canvas (daemon/output/strip.hpp), which bakes this in before the
// frame goes to a sink, so every sink — real strip, on-screen viewer, or a
// recording streamed to the receiver — sees the same corrected pixels.
//
// The table is kept at 8 extra fractional bits (a uint16 = output * 256)
// so the final 8-bit round-down can be dithered: see map() below. Without
// this, gamma + a low brightness collapse the 256 input codes onto only a
// few dozen output codes, so a slow gradient holds flat for many frames
// then jumps a whole code — visible stepping.
//
// *How* the round-down is biased is a pluggable DitherStrategy, picked by
// the `strip.dither` config key (firmware: STRIP_DITHER):
//
//   spatial  (default) — a static per-pixel/channel threshold, so adjacent
//            LEDs round opposite ways and a diffuser blends them optically
//            into the in-between value. No temporal component, so nothing
//            flickers; through a diffuser it resolves finer gradients than
//            the temporal scheme.
//   temporal — toggles each LED's LSB between frames so the eye averages it
//            over time. Resolves gradients on a bare (undiffused) strip, but
//            at low brightness the ±1 toggle is a 50–100% per-LED modulation
//            and at 30–60 fps it falls below flicker fusion, reading as a
//            low-level scintillation. Prefer it only without a diffuser.

// reverse the bits of a byte (0b00000001 -> 0b10000000)
inline uint8_t bitReverse8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// a dither yields, for one pixel/channel (and frame, if it dithers in time),
// an ordered threshold in [0,255] that biases ColorLut::map()'s round-down.
// Stateless; the concrete strategies below are shared singletons.
struct DitherStrategy
{
    virtual ~DitherStrategy() = default;
    virtual uint8_t threshold(uint32_t frame, int pixel, int channel) const = 0;
};

// spatial: static per pixel, no frame dependence. Bit-reversing the pixel
// index hands neighbours far-apart thresholds, so a uniform region dithers
// at high spatial frequency and the diffuser averages it cleanly.
//
// The same threshold is used for all three channels on purpose: a dim
// saturated colour (e.g. a 0.5-code teal floor) then rounds *up together*
// on the same pixels, so a lit pixel shows the whole colour. Giving each
// channel its own threshold instead splits that colour across pixels — you
// get isolated single-channel dots (a pure-green pixel where the floor is
// teal) — which reads as stray "wrong" colours rather than a dim wash.
struct SpatialDither : DitherStrategy
{
    uint8_t threshold(uint32_t, int pixel, int) const override
    {
        return bitReverse8((uint8_t)pixel);
    }
};

// temporal: bit-reverse the frame counter so consecutive frames land at
// opposite ends of the range (0, 128, 64, 192, ...) — a sub-LSB value is
// approximated within a handful of frames rather than over a slow ramp.
// The per-pixel offset keeps neighbours from toggling in lockstep; like the
// spatial strategy it shares one threshold across channels so a dim colour
// stays on-hue instead of fraying into single-channel dots.
struct TemporalDither : DitherStrategy
{
    uint8_t threshold(uint32_t frame, int pixel, int) const override
    {
        return (uint8_t)(bitReverse8((uint8_t)frame)
                         + (uint8_t)(pixel * 59));
    }
};

// shared stateless singletons, so copying a ColorLut just copies a pointer
// — no ownership, no heap, fine on the ESP32
inline const DitherStrategy& spatialDither()
{
    static const SpatialDither s;
    return s;
}
inline const DitherStrategy& temporalDither()
{
    static const TemporalDither t;
    return t;
}

// map a `strip.dither` config string to a strategy; unknown -> spatial
inline const DitherStrategy& ditherStrategyByName(const char* name)
{
    return (name && strcmp(name, "temporal") == 0) ? temporalDither()
                                                    : spatialDither();
}

class ColorLut
{
public:
    ColorLut()
    {
        brightness_ = 1.0f;
        gamma_[0] = gamma_[1] = gamma_[2] = 2.2f;
        wb_[0] = wb_[1] = wb_[2] = 1.0f;
        rebuild();
    }

    void setBrightness(float b)
    {
        if (b < 0) b = 0;
        if (b > 1) b = 1;
        brightness_ = b;
        rebuild();
    }

    float brightness() const { return brightness_; }

    // one value broadcasts to every channel
    void setGamma(float g) { setGamma(g, g, g); }

    // a per-channel gamma reshapes one channel's midtones without
    // touching its endpoints, correcting a tint that only shows at some
    // brightness levels (e.g. dim whites drifting blue)
    void setGamma(float r, float g, float b)
    {
        gamma_[0] = r > 0 ? r : 1.0f;
        gamma_[1] = g > 0 ? g : 1.0f;
        gamma_[2] = b > 0 ? b : 1.0f;
        rebuild();
    }

    // per-channel linear gain from an RRGGBB "white": the channel the
    // filter passes too strongly sits below 0xFF to pull it down. Linear
    // and level-independent, so balance holds from dim to full
    void setWhiteBalance(uint32_t rgb)
    {
        wb_[0] = ((rgb >> 16) & 0xFF) / 255.0f;
        wb_[1] = ((rgb >> 8) & 0xFF) / 255.0f;
        wb_[2] = (rgb & 0xFF) / 255.0f;
        rebuild();
    }

    // select how map() dithers the round-down: by name (config/firmware
    // string) or with a strategy directly. Independent of the LUT contents,
    // so no rebuild() is needed.
    void setDither(const char* name) { dither_ = &ditherStrategyByName(name); }
    void setDither(const DitherStrategy& d) { dither_ = &d; }

    // channel 0=R, 1=G, 2=B. Rounds the high-precision entry to nearest.
    uint8_t map(int channel, uint8_t value) const
    {
        return (uint8_t)((lut_[channel][value] + 128) >> 8);
    }

    // dithered variant: the active strategy biases the round-down by a
    // per-pixel (and, for temporal, per-frame) threshold instead of
    // rounding. With the threshold uniform over [0,255] the result averages
    // exactly to the true sub-code value, so a slow gradient glides instead
    // of stepping — across space or time, per the strategy.
    uint8_t map(int channel, uint8_t value, uint32_t frame, int pixel) const
    {
        return (uint8_t)((lut_[channel][value]
                          + dither_->threshold(frame, pixel, channel)) >> 8);
    }

private:
    // gamma first, then the white-balance gain and brightness scale
    // linear light; per channel so balance is independent of level
    void rebuild()
    {
        // store output * 256 so map() can dither the low 8 bits away
        for (int c = 0; c < 3; c++)
            for (int v = 0; v < 256; v++)
                lut_[c][v] = (uint16_t)(powf(v / 255.0f, gamma_[c])
                                * wb_[c] * brightness_ * 255.0f * 256.0f + 0.5f);
    }

    float brightness_;
    float gamma_[3];
    float wb_[3];
    uint16_t lut_[3][256];

    // flicker-free spatial dither by default; copying a ColorLut copies this
    // pointer, both pointing at the same shared singleton
    const DitherStrategy* dither_ = &spatialDither();
};
