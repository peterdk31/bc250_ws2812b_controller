#include <math.h>
#include "effect.hpp"

// whole-strip pulse for "something is wrong"
//
// config:
//   color              RRGGBB (default ff0000)
//   pulses_per_second  pulse rate (default 2)
class Alarm : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        uint32_t color = cfg.getColor("color", 0xff0000);

        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;

        speed = cfg.getFloat("pulses_per_second", 2.0f);
    }

    void render(Strip& strip, float t) override
    {
        float pulse = 0.5f + 0.5f * sinf(t * speed * 2.0f * (float)M_PI);

        // bottoms out at ~16% rather than dark, so the strip keeps
        // reading as "alarming" through the whole cycle
        float v = (40.0f + 215.0f * pulse) / 255.0f;

        for (int i = 0; i < strip.size(); i++)
            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
    }

private:
    uint8_t r = 255, g = 0, b = 0;
    float speed = 2.0f;
};

REGISTER_EFFECT("alarm", Alarm)
