#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "effect.hpp"
#include "hwmon.hpp"

// CPU and GPU load as two bars growing from the center of the strip
// outward — CPU toward pixel 0, GPU toward the last pixel — green at
// the center through yellow to red at the ends
//
// config:
//   smoothing_seconds  load smoothing time constant (default 0.5)
class Load : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        smoothing = cfg.getFloat("smoothing_seconds", 0.5f);

        if (!gpuLoad.available())
            fprintf(stderr, "load: no gpu load source found yet, "
                            "will keep looking\n");

        hwmon::readCpuCounters(prevBusy, prevTotal);
    }

    void render(WS2812Serial& strip, float) override
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

            // this pixel's center as distance from the strip center
            // 0..1; lit when the side's bar reaches the pixel's
            // middle (its far edge would make the outermost pixel
            // demand exactly 100%, which smoothing alone never
            // reaches), colored by how far out it sits
            float d = cpuSide ? (half - i - 0.5f) / half
                              : (i + 0.5f - half) / half;

            if (d > (cpuSide ? cpu : gpu))
            {
                strip.setPixel(i, 0, 0, 0);
                continue;
            }

            float r = d * 2.0f;
            float g = (1.0f - d) * 2.0f;

            if (r > 1) r = 1;
            if (g > 1) g = 1;

            strip.setPixel(i, (uint8_t)(r * 255), (uint8_t)(g * 255), 0);
        }
    }

    int frameDelayMs() const override { return 100; }

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

    float smoothing = 0.5f;
    float cpu = 0;
    float gpu = 0;

    hwmon::GpuLoad gpuLoad;
    unsigned long long prevBusy = 0;
    unsigned long long prevTotal = 0;
};

REGISTER_EFFECT("load", Load)
