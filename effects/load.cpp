#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "effect.hpp"
#include "color.hpp"
#include "hwmon.hpp"

// CPU and GPU load as two bars growing from the center of the strip
// outward — CPU toward pixel 0, GPU toward the last pixel — colored
// along a hue ramp from center_color to edge_color (the defaults
// sweep green through yellow to red)
//
// config:
//   smoothing_seconds  load smoothing time constant (default 0.5)
//   center_color       RRGGBB at the strip center (default 00ff00)
//   edge_color         RRGGBB at the strip ends (default ff0000)
class Load : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        ramp = color::Ramp(cfg.getColor("center_color", 0x00ff00),
                           cfg.getColor("edge_color", 0xff0000));

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

            uint8_t r, g, b;
            ramp.at(d, r, g, b);
            strip.setPixel(i, r, g, b);
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

    color::Ramp ramp;
    float smoothing = 0.5f;
    float cpu = 0;
    float gpu = 0;

    hwmon::GpuLoad gpuLoad;
    unsigned long long prevBusy = 0;
    unsigned long long prevTotal = 0;
};

REGISTER_EFFECT("load", Load)
