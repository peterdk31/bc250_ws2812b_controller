#include <math.h>
#include "effect.hpp"

class Rainbow : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        speed = cfg.getFloat("cycles_per_second", 0.625f);
    }

    void render(WS2812Serial& strip, float t) override
    {
        float offset = t * speed;

        for (int i = 0; i < strip.size(); i++)
        {
            float h = fmodf(((float)i / strip.size()) + offset, 1.0f);

            uint8_t r, g, b;
            hsv_to_rgb(h, r, g, b);

            strip.setPixel(i, r, g, b);
        }
    }

private:
    float speed = 0.625f;

    static void hsv_to_rgb(float h, uint8_t &r, uint8_t &g, uint8_t &b)
    {
        float s = 1.0f, v = 1.0f;

        float c = v * s;
        float x = c * (1 - fabsf(fmodf(h * 6, 2) - 1));
        float m = v - c;

        float rp, gp, bp;

        int i = (int)(h * 6);

        switch (i)
        {
            case 0: rp = c; gp = x; bp = 0; break;
            case 1: rp = x; gp = c; bp = 0; break;
            case 2: rp = 0; gp = c; bp = x; break;
            case 3: rp = 0; gp = x; bp = c; break;
            case 4: rp = x; gp = 0; bp = c; break;
            default: rp = c; gp = 0; bp = x; break;
        }

        r = (rp + m) * 255;
        g = (gp + m) * 255;
        b = (bp + m) * 255;
    }
};

REGISTER_EFFECT("rainbow", Rainbow)
