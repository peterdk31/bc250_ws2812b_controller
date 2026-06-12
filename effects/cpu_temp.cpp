#include <stdio.h>
#include "effect.hpp"
#include "color.hpp"
#include "hwmon.hpp"

// bar graph of a hwmon temperature sensor, colored along a hue ramp
// from cold_color to hot_color (the defaults sweep blue → green →
// yellow → red)
//
// config:
//   sensors     ordered chip:label candidates, first one present wins;
//               a bare chip name means its temp1_input,
//               e.g. "sensors": ["k10temp:Tctl", "nct6686:CPU", "nct6686"]
//   temp_min    °C where the bar starts (default 40)
//   temp_max    °C where the bar is full (default 85)
//   cold_color  RRGGBB at the bar's start (default 0000ff)
//   hot_color   RRGGBB at the bar's end (default ff0000)
class CpuTemp : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        ramp = color::Ramp(cfg.getColor("cold_color", 0x0000ff),
                           cfg.getColor("hot_color", 0xff0000));

        tempMin = cfg.getFloat("temp_min", 40.0f);
        tempMax = cfg.getFloat("temp_max", 85.0f);

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

    void render(WS2812Serial& strip, float) override
    {
        float temp = hwmon::readTemp(sensorPath);
        int leds = strip.size();

        float norm = (temp - tempMin) / (tempMax - tempMin);
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;

        int lit = (int)(norm * leds);

        for (int i = 0; i < leds; i++)
        {
            if (i < lit)
            {
                float pos = (float)i / leds;
                uint8_t r, g, b;
                ramp.at(pos, r, g, b);
                strip.setPixel(i, r, g, b);
            }
            else
            {
                strip.setPixel(i, 0, 0, 0);
            }
        }
    }

    int frameDelayMs() const override { return 200; }

private:
    std::string sensorPath;
    color::Ramp ramp;
    float tempMin = 40.0f;
    float tempMax = 85.0f;
};

REGISTER_EFFECT("cpu_temp", CpuTemp)
