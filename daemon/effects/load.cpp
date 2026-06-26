#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "hwmon.hpp"

// CPU and GPU load as two soft bars growing from the center of the strip
// outward — CPU toward pixel 0, GPU toward the last pixel. Instead of a
// hard thermal ramp, the lit pixels are washed with slow aurora-style
// color curtains plus a gentle brightness shimmer, so a bar pinned at
// 100% (a GPU under a game) keeps drifting and breathing rather than
// sitting as a static block. The bar tip fades across a pixel instead of
// snapping on and off.
//
// config:
//   smoothing_seconds  load smoothing time constant (default 0.7)
//   palette            comma-separated stops the wash walks
//                      (default cool teal -> blue -> violet)
//   speed              drift / shimmer rate multiplier (default 1.4)
//   noise              flow/noise blend 0 (sine flow) .. 1 (noise) (default 0.3)
//   floor_brightness   dim wash on the unlit track 0..1 (default 0.04)
class Load : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        smoothing = cfg.getFloat("smoothing_seconds", 0.7f);
        palette = color::Gradient(cfg.get("palette", "00e0c0,2060ff,a040ff"));
        speed = cfg.getFloat("speed", 1.4f);
        noiseMix = cfg.getFloat("noise", 0.3f);
        floorLevel = cfg.getFloat("floor_brightness", 0.04f);

        if (!gpuLoad.available())
            fprintf(stderr, "load: no gpu load source found yet, "
                            "will keep looking\n");

        hwmon::readCpuCounters(prevBusy, prevTotal);
    }

    void render(Strip& strip, float t) override
    {
        float dt = frameDelayMs() / 1000.0f;
        float alpha = smoothing > 0 ? 1.0f - expf(-dt / smoothing) : 1.0f;

        cpu += alpha * (readCpuLoad() - cpu);
        gpu += alpha * (readGpuLoad() - gpu);

        int leds = strip.size();
        float half = leds / 2.0f;

        for (int i = 0; i < leds; i++)
        {
            bool cpuSide = i < half;

            // this pixel's center as distance from the strip center, 0..1
            float d = cpuSide ? (half - i - 0.5f) / half
                              : (i + 0.5f - half) / half;

            float level = cpuSide ? cpu : gpu;

            // soft tip: full inside the bar, fading over the single pixel
            // straddling its end (distance in pixels is d * half). `live`
            // is the same ramp before flooring: 1 inside the bar, 0 deep in
            // the unlit track, crossing 0..1 across the tip pixel.
            float live = (level - d) * half + 0.5f;
            if (live < 0.0f) live = 0.0f;
            if (live > 1.0f) live = 1.0f;

            float fill = live;

            // the unlit track keeps a faint wash rather than going black,
            // so the bar fades down into a living floor instead of an edge
            if (fill < floorLevel) fill = floorLevel;

            // spatial coordinate for the wash/shimmer mirrored about the
            // center (distance outward, 0..1), so both halves flow
            // symmetrically rather than the pattern sweeping straight
            // across the strip in one screen direction
            float x = d;

            // freeze the animation in the unlit track (scale time by `live`):
            // the 0.04 floor sits right on the dither's single-code boundary,
            // so any frame-to-frame motion there flips pixels on and off and
            // twinkles. A static floor dithers to fixed dots — a calm dim
            // glow — while the lit bar and its tip keep the full living wash.
            float at = t * live;

            // aurora-style wash walking the palette: a flow field blended
            // with value noise, so the color drifts and never repeats
            float w = motion::mix(motion::flow(x, at, speed),
                                  motion::noise(x, at, speed), noiseMix);

            // a separate slow field shimmers the brightness, so even a
            // full bar is always gently moving
            float v = 0.6f + 0.4f * motion::shimmer(x, at, speed);

            uint8_t r, g, b;
            palette.at(w, r, g, b);

            float k = v * fill;
            strip.setPixel(i, (uint8_t)(r * k), (uint8_t)(g * k),
                           (uint8_t)(b * k));
        }
    }

private:
    // busy share of /proc/stat ticks since the previous call, 0..1
    float readCpuLoad()
    {
        unsigned long long busy, total;

        if (!hwmon::readCpuCounters(busy, total) || total <= prevTotal)
            return cpu;

        float load = (float)(busy - prevBusy) / (float)(total - prevTotal);

        prevBusy = busy;
        prevTotal = total;

        return load < 0 ? 0 : load > 1 ? 1 : load;
    }

    float readGpuLoad()
    {
        return gpuLoad.readPercent() / 100;
    }

    float smoothing = 0.7f;
    color::Gradient palette;
    float speed = 1.4f;
    float noiseMix = 0.3f;
    float floorLevel = 0.04f;
    float cpu = 0;
    float gpu = 0;

    hwmon::GpuLoad gpuLoad;
    unsigned long long prevBusy = 0;
    unsigned long long prevTotal = 0;
};

REGISTER_EFFECT("load", Load)
