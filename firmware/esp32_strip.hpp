#pragma once

#include <stdint.h>
#include <Freenove_WS2812_Lib_for_ESP32.h>
#include "color_lut.hpp"

// strip-level correction baked in at flash time (see the Makefile, which
// fills these from the host config). They mirror what the daemon applies
// so a standalone animation on the receiver is tinted and dimmed exactly
// like the frames the host will later stream.
#ifndef STRIP_BRIGHTNESS
#define STRIP_BRIGHTNESS 0.2f
#endif
#ifndef STRIP_WHITE_BALANCE
#define STRIP_WHITE_BALANCE 0xFFFFFF
#endif
// a single gamma is fine for the receiver's animations (they're not
// gradients tuned to a per-channel falloff like some host effects)
#ifndef STRIP_GAMMA
#define STRIP_GAMMA 2.2f
#endif

// the receiver-side `Strip` backend: the same setPixel/size surface the
// effects render against, but it writes through the shared color LUT into
// the Freenove driver instead of into a serial buffer. A thin view over a
// strip the firmware already owns and a shared LUT, so it's cheap to
// construct per frame.
class Esp32Strip
{
public:
    // frame is the receiver's standalone frame counter; it phases the
    // temporal dither (see ColorLut) so slow gradients don't step. Defaults
    // to 0 for callers that render a single static frame.
    Esp32Strip(Freenove_ESP32_WS2812* strip, int count, const ColorLut& lut,
               uint32_t frame = 0)
        : strip_(strip), count_(count), lut_(lut), frame_(frame) {}

    void beginFrame() {} // no framing on this side; here for API parity

    void setPixel(int i, uint8_t r, uint8_t g, uint8_t b)
    {
        if (i < 0 || i >= count_ || !strip_)
            return;

        strip_->setLedColorData(i,
                                lut_.map(0, r, ditherThreshold(frame_, i, 0)),
                                lut_.map(1, g, ditherThreshold(frame_, i, 1)),
                                lut_.map(2, b, ditherThreshold(frame_, i, 2)));
    }

    int size() const { return count_; }

private:
    Freenove_ESP32_WS2812* strip_;
    int count_;
    const ColorLut& lut_;
    uint32_t frame_;
};
