#include <math.h>
#include "effect.hpp"

// whole-strip red pulse for "something is wrong"
class Alarm : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        speed = cfg.getFloat("pulses_per_second", 2.0f);
    }

    void render(WS2812Serial& strip, float t) override
    {
        float pulse = 0.5f + 0.5f * sinf(t * speed * 2.0f * (float)M_PI);
        uint8_t r = (uint8_t)(40 + 215 * pulse);

        for (int i = 0; i < strip.size(); i++)
            strip.setPixel(i, r, 0, 0);
    }

private:
    float speed = 2.0f;
};

REGISTER_EFFECT("alarm", Alarm)
