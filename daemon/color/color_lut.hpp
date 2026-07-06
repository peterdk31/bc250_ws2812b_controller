#pragma once

#include <stdint.h>
#include <math.h>

// the per-channel color correction the strip applies to every pixel:
// gamma curves each channel, a white-balance gain rescales it, then a
// global brightness scales the whole thing — all folded into one
// 3x256 lookup table so applying it per pixel is a single array read.
//
// used by the host canvas (daemon/output/strip.hpp), which bakes this in
// before the frame goes to a sink, so every sink — real strip, on-screen
// viewer, or a recording streamed to the receiver — sees the same corrected
// pixels.
//
// The table keeps 8 extra fractional bits (a uint16 = strip code * 256, the
// wire's 8.8 fixed point — see common/protocol.hpp), and those bits ride the
// wire whole: the receiver rounds them away with a temporal dither at its own
// strip-refresh rate, several times faster than frames arrive
// (common/dither.hpp). Without the fraction, gamma + a low brightness
// collapse the 256 input codes onto only a few dozen output codes, so a slow
// gradient holds flat for many frames then jumps a whole code — visible
// stepping; and any rounding done here, at frame rate, either steps the same
// way or flickers. Shipping the fraction lets the one place fast enough to
// hide the round-down make it.
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

    // channel 0=R, 1=G, 2=B: the corrected value in 8.8 fixed point
    // (0..0xFF00), exactly what the wire carries
    uint16_t map16(int channel, uint8_t value) const
    {
        return lut_[channel][value];
    }

private:
    // gamma first, then the white-balance gain and brightness scale
    // linear light; per channel so balance is independent of level
    void rebuild()
    {
        // store output * 256, keeping the fraction for the receiver to dither
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
