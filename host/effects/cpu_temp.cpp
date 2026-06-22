#include <math.h>
#include <stdio.h>
#include "effect.hpp"
#include "color.hpp"
#include "hwmon.hpp"

// bar graph of a hwmon temperature sensor, colored along a hue ramp from
// cold_color to hot_color (the defaults sweep blue → green → yellow →
// red). The thermal ramp is kept — here the color genuinely means
// temperature — but the bar tip now fades across a pixel instead of
// snapping, and the whole bar carries a slow brightness shimmer so a
// steady temperature reads as a living bar rather than a static block.
//
// config:
//   sensors     ordered chip:label candidates, first one present wins;
//               a bare chip name means its temp1_input,
//               e.g. "sensors": ["k10temp:Tctl", "nct6686:CPU", "nct6686"]
//   temp_min    °C where the bar starts (default 40)
//   temp_max    °C where the bar is full (default 85)
//   cold_color  RRGGBB at the bar's start (default 0000ff)
//   hot_color   RRGGBB at the bar's end (default ff0000)
//   speed       shimmer rate multiplier (default 1.0)
//   floor_brightness  dim cold glow on the unlit track 0..1 (default 0.04)
class CpuTemp : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 33);

        ramp = color::Ramp(cfg.getColor("cold_color", 0x0000ff),
                           cfg.getColor("hot_color", 0xff0000));

        tempMin = cfg.getFloat("temp_min", 40.0f);
        tempMax = cfg.getFloat("temp_max", 85.0f);
        speed = cfg.getFloat("speed", 1.0f);
        floorLevel = cfg.getFloat("floor_brightness", 0.04f);

        // the unlit track glows faintly in the cold color, so the strip
        // reads as alive even with the bar empty
        ramp.at(0.0f, coldR, coldG, coldB);

        if (tempMax <= tempMin)
            tempMax = tempMin + 1;

        std::string spec = cfg.get("sensors", hwmon::DEFAULT_SENSORS);

        sensorPath = hwmon::findSensorFromSpec(spec);

        if (sensorPath.empty())
        {
            fprintf(stderr, "cpu_temp: no sensor from '%s' found; available chips:\n",
                    spec.c_str());
            hwmon::listChips();
        }
        else
        {
            fprintf(stderr, "cpu_temp: using %s\n", sensorPath.c_str());
        }
    }

    void render(WS2812Serial& strip, float t) override
    {
        float temp = hwmon::readTemp(sensorPath);
        int leds = strip.size();

        float norm = (temp - tempMin) / (tempMax - tempMin);
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;

        // fractional lit length in pixels, so the bar's end can sit
        // mid-pixel and fade there
        float level = norm * leds;

        for (int i = 0; i < leds; i++)
        {
            float fill = level - i;
            if (fill > 1.0f) fill = 1.0f;

            float pos = (float)i / leds;

            // slow brightness shimmer travelling along the bar
            float s = 0.7f + 0.3f * sinf(pos * 3.7f + t * 0.23f * speed + 1.7f);

            if (fill <= 0.0f)
            {
                // unlit track: faint cold glow rather than black
                float k = floorLevel * s;
                strip.setPixel(i, (uint8_t)(coldR * k), (uint8_t)(coldG * k),
                               (uint8_t)(coldB * k));
                continue;
            }

            uint8_t r, g, b;
            ramp.at(pos, r, g, b);
            float k = fill * s;

            strip.setPixel(i, (uint8_t)(r * k), (uint8_t)(g * k),
                           (uint8_t)(b * k));
        }
    }

private:
    std::string sensorPath;
    color::Ramp ramp;
    float tempMin = 40.0f;
    float tempMax = 85.0f;
    float speed = 1.0f;
    float floorLevel = 0.04f;
    uint8_t coldR = 0, coldG = 0, coldB = 255;
};

REGISTER_EFFECT("cpu_temp", CpuTemp)
