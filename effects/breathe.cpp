#include <math.h>
#include "effect.hpp"

// whole-strip single color with brightness on a slow sine
//
// config:
//   color           RRGGBB (default ff7818, warm amber)
//   period_seconds  seconds per breath (default 5)
//   min_brightness  floor 0..1 so the strip never fully blanks (default 0.05)
class Breathe : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        uint32_t color = cfg.getColor("color", 0xff7818);

        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;

        period = cfg.getFloat("period_seconds", 5.0f);
        if (period <= 0) period = 5.0f;

        minLevel = cfg.getFloat("min_brightness", 0.05f);
    }

    void render(WS2812Serial& strip, float t) override
    {
        float breath = 0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI / period);
        float v = minLevel + (1.0f - minLevel) * breath;

        for (int i = 0; i < strip.size(); i++)
            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
    }

    int frameDelayMs() const override { return 33; }

private:
    uint8_t r = 255, g = 120, b = 24;
    float period = 5.0f;
    float minLevel = 0.05f;
};

REGISTER_EFFECT("breathe", Breathe)
