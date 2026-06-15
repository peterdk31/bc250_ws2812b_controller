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

    // channel 0=R, 1=G, 2=B
    uint8_t map(int channel, uint8_t value) const
    {
        return lut_[channel][value];
    }

private:
    // gamma first, then the white-balance gain and brightness scale
    // linear light; per channel so balance is independent of level
    void rebuild()
    {
        for (int c = 0; c < 3; c++)
            for (int v = 0; v < 256; v++)
                lut_[c][v] = (uint8_t)(powf(v / 255.0f, gamma_[c])
                                       * wb_[c] * brightness_ * 255.0f + 0.5f);
    }

    float brightness_;
    float gamma_[3];
    float wb_[3];
    uint8_t lut_[3][256];
};
