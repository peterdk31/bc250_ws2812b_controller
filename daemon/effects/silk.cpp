#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// tide with the gradient run through a drifting warp field: instead of the
// palette scrolling past in a straight sweep, the color bands stretch,
// buckle and locally fold back on themselves like fabric moving in a slow
// breeze. A soft sheen sampled at the same warped coordinate rides the
// folds, so highlights sit on the creases rather than sliding independently.
// The palette wraps, so loop it (first stop == last) for a seamless flow.
//
// config:
//   palette     comma-separated stops (default looped silk:
//               "3a1078,c03080,ff9040,20b0c0,3a1078")
//   speed       drift rate multiplier (default 1.0)
//   span        full palette sweeps across the strip (default 1.0)
//   fold        warp depth — how far the noise field displaces the
//               gradient, in palette sweeps (default 0.5); 0 reproduces
//               plain tide
//   fold_scale  how many warp cells span the strip (default 2.0); higher
//               gives busier, smaller ripples
//   sheen       brightness depth of the highlight riding the folds, 0..1
//               (default 0.3); the floor never drops below 1 - sheen
class Silk : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette",
                                          "3a1078,c03080,ff9040,20b0c0,3a1078"));
        speed = cfg.getFloat("speed", 1.0f);
        span = cfg.getFloat("span", 1.0f);
        fold = cfg.getFloat("fold", 0.5f);
        foldScale = cfg.getFloat("fold_scale", 2.0f);
        sheen = cfg.getFloat("sheen", 0.3f);
        if (sheen < 0.0f) sheen = 0.0f;
        if (sheen > 1.0f) sheen = 1.0f;
    }

    void render(Strip& strip, float t) override
    {
        // the band scale breathes slowly, as in tide
        float breath = 1.0f + 0.2f * sinf(t * 0.05f * speed);

        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // the warp drifts a little faster than the scroll, so folds
            // travel through the gradient instead of being carried by it
            float warp = (motion::noise(x, t, 1.6f * speed, foldScale)
                          - 0.5f) * fold;

            float p = x * span * breath - t * 0.045f * speed + warp;
            p -= floorf(p);

            uint8_t r, g, b;
            palette.at(p, r, g, b);

            // sheen sampled at the warped coordinate: where the fabric
            // folds, the highlight catches
            float v = 1.0f - sheen
                    + sheen * motion::shimmer(x + warp * 2.0f, t, speed);

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

private:
    color::Gradient palette;
    float speed = 1.0f;
    float span = 1.0f;
    float fold = 0.5f;
    float foldScale = 2.0f;
    float sheen = 0.3f;
};

REGISTER_EFFECT("silk", Silk)
