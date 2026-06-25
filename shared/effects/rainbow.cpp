#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// scrolling hue cycle, lit from within: a slow shimmer field dips the
// brightness and a second field eases the saturation up and down, so the
// scroll drifts and breathes instead of reading as a flat conveyor belt.
//
// config:
//   cycles_per_second hue scroll rate (default 0.9)
//   shimmer_depth     how far the brightness dips 0..1 (default 0.18)
//   sat_depth         how far the saturation eases off 0..1 (default 0.15)
class Rainbow : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        speed = cfg.getFloat("cycles_per_second", 0.9f);
        shimmerDepth = cfg.getFloat("shimmer_depth", 0.18f);
        satDepth = cfg.getFloat("sat_depth", 0.15f);
    }

    void render(Strip& strip, float t) override
    {
        float offset = t * speed;

        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            float h = fmodf(x + offset, 1.0f);
            float v = 1.0f - shimmerDepth + shimmerDepth * motion::shimmer(x, t);
            float s = 1.0f - satDepth * (1.0f - motion::shimmer(x, t, 1.0f, 4.0f));

            uint8_t r, g, b;
            color::toRgb(h, s, v, r, g, b);

            strip.setPixel(i, r, g, b);
        }
    }

private:
    float speed = 0.9f;
    float shimmerDepth = 0.18f;
    float satDepth = 0.15f;
};

REGISTER_EFFECT("rainbow", Rainbow)
