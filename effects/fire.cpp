#include <stdlib.h>
#include "effect.hpp"

// per-LED candle flicker: each pixel's heat does a random walk and is
// mapped through a black → red → orange palette
//
// config:
//   speed     flicker rate multiplier (default 1.0)
//   min_heat  palette floor 0..1 so pixels never go fully dark (default 0.25)
class Fire : public Effect
{
public:
    void init(const EffectConfig& cfg, int leds) override
    {
        speed = cfg.getFloat("speed", 1.0f);
        minHeat = cfg.getFloat("min_heat", 0.25f);

        heat.resize(leds);
        target.resize(leds);

        for (int i = 0; i < leds; i++)
        {
            heat[i] = randf();
            target[i] = randf();
        }
    }

    void render(WS2812Serial& strip, float) override
    {
        float step = 0.06f * speed;

        for (int i = 0; i < strip.size(); i++)
        {
            float d = target[i] - heat[i];

            if (d > step)
                heat[i] += step;
            else if (d < -step)
                heat[i] -= step;
            else
                target[i] = randf();

            uint8_t r, g, b;
            heat_to_rgb(minHeat + (1.0f - minHeat) * heat[i], r, g, b);
            strip.setPixel(i, r, g, b);
        }
    }

    int frameDelayMs() const override { return 33; }

private:
    float speed = 1.0f;
    float minHeat = 0.25f;
    std::vector<float> heat;
    std::vector<float> target;

    static float randf()
    {
        return (float)rand() / (float)RAND_MAX;
    }

    // black → red → orange; green and blue are held back so the top
    // of the range stays amber instead of going yellow/white
    static void heat_to_rgb(float h, uint8_t &r, uint8_t &g, uint8_t &b)
    {
        if (h < 0) h = 0;
        if (h > 1) h = 1;

        float r1 = h * 3.0f;
        if (r1 > 1) r1 = 1;

        float g1 = (h - 0.4f) / 0.6f;
        if (g1 < 0) g1 = 0;
        g1 *= 0.55f;

        float b1 = (h - 0.85f) / 0.15f;
        if (b1 < 0) b1 = 0;
        b1 *= 0.15f;

        r = (uint8_t)(r1 * 255);
        g = (uint8_t)(g1 * 255);
        b = (uint8_t)(b1 * 255);
    }
};

REGISTER_EFFECT("fire", Fire)
