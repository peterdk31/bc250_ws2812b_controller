#include <math.h>
#include "effect.hpp"

// gentle power-on: the charge color blooms outward from the middle of
// the strip to both ends, holds for a beat, then fades to black, handing
// a dark strip to the rule engine. Reports finished so the playlist
// advances.
//
// config:
//   duration_seconds  total run time (default 5)
//   color             bloom color RRGGBB (default 0028ff)
class Boot : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        duration = cfg.getFloat("duration_seconds", 5.0f);
        if (duration <= 0) duration = 5.0f;

        uint32_t color = cfg.getColor("color", 0x0028ff);

        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;
    }

    void render(Strip& strip, float t) override
    {
        elapsed = t;

        int leds = strip.size();
        float center = (leds - 1) * 0.5f;
        float maxRadius = center + 1.0f; // +1 so the end pixels reach full

        float u = t / duration;
        if (u > 1) u = 1;

        float radius = maxRadius; // how far the bloom has opened, in pixels
        float dim = 1.0f;         // whole-strip brightness, for the fade

        if (u < BLOOM_END)
        {
            // smoothstep so the bloom eases open and brakes into the hold
            float p = u / BLOOM_END;
            radius = p * p * (3.0f - 2.0f * p) * maxRadius;
        }
        else if (u >= FADE_START)
        {
            // quadratic fade: quick drop, soft landing into black
            float k = 1.0f - (u - FADE_START) / (1.0f - FADE_START);
            dim = k * k;
        }

        for (int i = 0; i < leds; i++)
        {
            // 1-pixel-wide soft edge keeps the bloom front from stepping
            float cov = radius - fabsf(i - center) + 0.5f;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;

            float v = cov * dim;

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

    bool finished() const override { return elapsed >= duration; }

private:
    // phase boundaries as fractions of the total duration: bloom open,
    // brief full hold, then fade
    static constexpr float BLOOM_END = 0.6f;
    static constexpr float FADE_START = 0.72f;

    uint8_t r = 0, g = 40, b = 255;
    float duration = 5.0f;
    float elapsed = 0.0f;
};

REGISTER_EFFECT("boot", Boot)
