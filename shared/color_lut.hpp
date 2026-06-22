#pragma once

#include <stdint.h>
#include <math.h>

// the per-channel color correction the strip applies to every pixel:
// gamma curves each channel, a white-balance gain rescales it, then a
// global brightness scales the whole thing — all folded into one
// 3x256 lookup table so applying it per pixel is a single array read.
//
// shared by the host transport (ws2812_serial.hpp, which bakes this in
// before sending) and the ESP32's standalone animations (esp32_strip.hpp),
// so a breathe rendered on the receiver matches what the daemon sends.
//
// The table is kept at 8 extra fractional bits (a uint16 = output * 256)
// so the final 8-bit round-down can be temporally dithered: see map() and
// ditherThreshold() below. Without this, gamma + a low brightness collapse
// the 256 input codes onto only a few dozen output codes, so a slow
// gradient holds flat for many frames then jumps a whole code — visible
// stepping. Dithering toggles the LSB between frames so the eye averages
// it back to the in-between value.

// reverse the bits of a byte (0b00000001 -> 0b10000000)
inline uint8_t bitReverse8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// ordered temporal dither threshold in [0,255] for one pixel/channel on
// one frame. The frame counter is bit-reversed so consecutive frames land
// at opposite ends of the range (0, 128, 64, 192, ...): a sub-LSB value is
// approximated within a handful of frames rather than over a slow ramp.
// A per-pixel/channel offset keeps neighbours from toggling in lockstep,
// which would read as a whole-strip flicker instead of dither.
inline uint8_t ditherThreshold(uint32_t frame, int pixel, int channel)
{
    return (uint8_t)(bitReverse8((uint8_t)frame)
                     + (uint8_t)(pixel * 59 + channel * 131));
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

    // channel 0=R, 1=G, 2=B. Rounds the high-precision entry to nearest.
    uint8_t map(int channel, uint8_t value) const
    {
        return (uint8_t)((lut_[channel][value] + 128) >> 8);
    }

    // dithered variant: bias the round-down by a per-pixel/per-frame
    // threshold (see ditherThreshold) instead of rounding. With the
    // threshold uniform over [0,255] the result averages exactly to the
    // true sub-code value, so a slow gradient glides instead of stepping.
    uint8_t map(int channel, uint8_t value, uint8_t dither) const
    {
        return (uint8_t)((lut_[channel][value] + dither) >> 8);
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
};
