#pragma once

#include <stdint.h>
#include <string.h>
#include "motion.hpp"

// The crossfade between animations, owned by whoever drives a display: the
// ESP32 firmware for the real strip, and host/virtual_strip.cpp for the
// on-screen preview. Both keep the last frame they showed; when a new animation
// begins (the per-frame `anim` id changed, or live frames take over from a
// recording replay) they begin() to freeze that outgoing frame, then run every
// subsequent incoming frame through apply() until the dissolve completes.
//
// This is the dissolve the daemon used to do itself, lifted into one shared
// place so the device and the preview can't drift — the same way protocol.hpp
// and receiver.hpp keep the wire format in sync. It's a smootherstep
// (motion::ease) over wall-clock ms, so the dissolve is the same length
// regardless of frame rate. Pure and heap-free: usable as-is on the ESP32.
//
// Frames are count*3 channel values in the wire's 8.8 fixed point (see
// common/protocol.hpp), so a dissolve keeps the sub-code precision the
// receiver's dithering renders — and the firmware re-blends per strip
// refresh, making the dissolve itself as smooth as the dither.
namespace fade
{

// caps the frozen-frame buffer; matches the receiver's pixel-frame ceiling
static const uint16_t kMaxLeds = 2048;

class Fader
{
public:
    // freeze `last` (count*3 8.8 values, the frame currently on screen) as the
    // outgoing frame and start a dissolve lasting durMs. A zero duration, no
    // outgoing frame, or an out-of-range count means "switch instantly" — the
    // fader stays inactive and apply() is a no-op.
    void begin(const uint16_t* last, uint16_t count, uint16_t durMs,
               uint32_t now)
    {
        if (!last || durMs == 0 || count == 0 || count > kMaxLeds)
        {
            active_ = false;
            return;
        }

        count_ = count;
        durMs_ = durMs;
        startMs_ = now;
        memcpy(from_, last, (size_t)count * 3 * sizeof(uint16_t));
        active_ = true;
    }

    // blend the incoming frame `px` (count*3 8.8 values) over the frozen one,
    // in place, by the eased fraction of durMs elapsed. Disarms when the
    // dissolve finishes — or if the geometry changed mid-fade — leaving `px`
    // as the fully faded-in (i.e. untouched) frame.
    void apply(uint16_t* px, uint16_t count, uint32_t now)
    {
        if (!active_)
            return;

        if (count != count_)
        {
            active_ = false; // geometry changed under us: abandon the dissolve
            return;
        }

        uint32_t elapsed = now - startMs_;

        if (elapsed >= durMs_)
        {
            active_ = false; // done — the incoming frame stands on its own
            return;
        }

        float a = motion::ease((float)elapsed / (float)durMs_);
        int n = (int)count * 3;

        for (int i = 0; i < n; i++)
        {
            float v = (float)from_[i] + ((float)px[i] - (float)from_[i]) * a;
            px[i] = (uint16_t)(v + 0.5f);
        }
    }

    bool active() const { return active_; }

private:
    uint16_t from_[(size_t)kMaxLeds * 3];
    uint16_t count_ = 0;
    uint16_t durMs_ = 0;
    uint32_t startMs_ = 0;
    bool active_ = false;
};

} // namespace fade
