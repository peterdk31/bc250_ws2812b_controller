#include <stdio.h>
#include "effect.hpp"
#include "hwmon.hpp"

// bar graph of a hwmon temperature sensor: blue → green → yellow → red
//
// config:
//   sensors   ordered chip:label candidates, first one present wins;
//             a bare chip name means its temp1_input,
//             e.g. "sensors": ["k10temp:Tctl", "nct6686:CPU", "nct6686"]
//   temp_min  °C where the bar starts (default 40)
//   temp_max  °C where the bar is full (default 85)
class CpuTemp : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
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
                temp_to_rgb(pos, r, g, b);
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
    float tempMin = 40.0f;
    float tempMax = 85.0f;

    // color: blue → green → yellow → red
    static void temp_to_rgb(float x, uint8_t &r, uint8_t &g, uint8_t &b)
    {
        if (x < 0) x = 0;
        if (x > 1) x = 1;

        float r1, g1, b1;

        if (x < 0.33f)
        {
            // blue → green
            float t = x / 0.33f;
            r1 = 0;
            g1 = t;
            b1 = 1.0f - t;
        }
        else if (x < 0.66f)
        {
            // green → yellow
            float t = (x - 0.33f) / 0.33f;
            r1 = t;
            g1 = 1.0f;
            b1 = 0;
        }
        else
        {
            // yellow → red
            float t = (x - 0.66f) / 0.34f;
            r1 = 1.0f;
            g1 = 1.0f - t;
            b1 = 0;
        }

        r = (uint8_t)(r1 * 255);
        g = (uint8_t)(g1 * 255);
        b = (uint8_t)(b1 * 255);
    }
};

REGISTER_EFFECT("cpu_temp", CpuTemp)
