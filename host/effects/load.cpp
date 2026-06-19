#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "effect.hpp"
#include "color.hpp"
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
//   hue_min            low end of the cool hue band 0..1 (default 0.45, teal)
//   hue_max            high end of the cool hue band 0..1 (default 0.83, purple)
//   speed              drift / shimmer rate multiplier (default 1.0)
class Load : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        smoothing = cfg.getFloat("smoothing_seconds", 0.7f);
        hueMin = cfg.getFloat("hue_min", 0.45f);
        hueMax = cfg.getFloat("hue_max", 0.83f);
        speed = cfg.getFloat("speed", 1.0f);

        if (!gpuLoad.available())
            fprintf(stderr, "load: no gpu load source found yet, "
                            "will keep looking\n");

        hwmon::readCpuCounters(prevBusy, prevTotal);
    }

    void render(WS2812Serial& strip, float t) override
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
            // straddling its end (distance in pixels is d * half)
            float fill = (level - d) * half + 0.5f;
            if (fill <= 0.0f)
            {
                strip.setPixel(i, 0, 0, 0);
                continue;
            }
            if (fill > 1.0f) fill = 1.0f;

            // spatial coordinate for the wash/shimmer mirrored about the
            // center (distance outward, 0..1), so both halves flow
            // symmetrically rather than the pattern sweeping straight
            // across the strip in one screen direction
            float x = d;

            // aurora-style hue curtains: two sines at unrelated
            // frequencies drifting in opposite directions, so the wash
            // never visibly repeats
            float w = 0.5f + 0.25f * sinf(x * 5.1f + t * 0.31f * speed)
                           + 0.25f * sinf(x * 2.3f - t * 0.17f * speed);
            float h = hueMin + (hueMax - hueMin) * w;

            // a separate slow field shimmers the brightness, so even a
            // full bar is always gently moving
            float v = 0.6f + 0.4f * sinf(x * 3.7f + t * 0.23f * speed + 1.7f);

            uint8_t r, g, b;
            color::toRgb(h, 1.0f, v * fill, r, g, b);
            strip.setPixel(i, r, g, b);
        }
    }

    int frameDelayMs() const override { return 33; }

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
    float hueMin = 0.45f;
    float hueMax = 0.83f;
    float speed = 1.0f;
    float cpu = 0;
    float gpu = 0;

    hwmon::GpuLoad gpuLoad;
    unsigned long long prevBusy = 0;
    unsigned long long prevTotal = 0;
};

REGISTER_EFFECT("load", Load)
