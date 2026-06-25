#include <math.h>
#include "effect.hpp"
#include "color.hpp"

// a smooth palette gradient sliding slowly along the strip while its scale
// gently breathes, so the color bands stretch and compress like a slow
// tide. More orderly than plasma — one continuous sweep of color drifting
// past. The palette wraps, so loop it (first stop == last) for a seamless
// scroll.
//
// config:
//   palette  comma-separated stops (default cool tide, looped:
//            "102060,1890d0,30d0a0,8040ff,102060")
//   speed    slide rate multiplier (default 1.4)
//   span     full palette sweeps across the strip (default 1.0)
class Tide : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette",
                                          "102060,1890d0,30d0a0,8040ff,102060"));
        speed = cfg.getFloat("speed", 1.4f);
        span = cfg.getFloat("span", 1.0f);
    }

    void render(Strip& strip, float t) override
    {
        // the band scale breathes slowly so the gradient stretches and
        // compresses instead of scrolling at a fixed rate
        float breath = 1.0f + 0.25f * sinf(t * 0.07f * speed);

        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            float p = x * span * breath - t * 0.05f * speed;
            p -= floorf(p);                       // wrap into 0..1

            uint8_t r, g, b;
            palette.at(p, r, g, b);

            strip.setPixel(i, r, g, b);
        }
    }

private:
    color::Gradient palette;
    float speed = 1.4f;
    float span = 1.0f;
};

REGISTER_EFFECT("tide", Tide)
