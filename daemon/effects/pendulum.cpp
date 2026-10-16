#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// pendulum wave: every LED is a pendulum, and each one further from the
// origin swings exactly one more time per period than its neighbour. They
// start in step — the whole strip swells as one — then drift apart into a
// travelling wave whose wavelength keeps shrinking, fold through standing
// waves, two- and three-way braids and an apparent tangle at mid-period,
// and then, as if by magic, march back into step at the end of the period
// and start over. Deterministic: the "chaos" is pure phase arithmetic.
//
// Each LED's light is its pendulum's closeness to the near end of its
// swing, sharpened so the bobs read as bright crests over a true-dark
// track. Color runs across the strip by distance from the origin, drifting
// slowly, so the crests change hue as the waves carry them outward.
//
// config:
//   palette        stops across the strip by distance from the origin
//                  (default looped "2040ff,00d0c0,a040ff,ff4090,2040ff")
//   period_seconds time for the whole pattern to realign (default 60)
//   swings         swings per period of the pendulum at the origin
//                  (default 15); each pixel outward adds one
//   sharp          crest sharpening exponent (default 2.5); higher gives
//                  thinner bobs over more dark
//   span           palette sweeps from the origin to the far end
//                  (default 1.0)
//   color_speed    palette drift in sweeps per minute (default 1.0)
//   origin         where the slowest pendulum hangs, 0..1 (default 0.0 =
//                  first LED); 0.5 mirrors the wave around the strip center
class Pendulum : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette",
                                          "2040ff,00d0c0,a040ff,ff4090,2040ff"));
        period = cfg.getFloat("period_seconds", 60.0f);
        if (period < 5.0f) period = 5.0f;
        swings = cfg.getFloat("swings", 15.0f);
        if (swings < 1.0f) swings = 1.0f;
        sharp = cfg.getFloat("sharp", 2.5f);
        if (sharp < 0.5f) sharp = 0.5f;
        span = cfg.getFloat("span", 1.0f);
        colorSpeed = cfg.getFloat("color_speed", 1.0f) / 60.0f;
        origin = cfg.getFloat("origin", 0.0f);
        if (origin < 0.0f) origin = 0.0f;
        if (origin > 1.0f) origin = 1.0f;
    }

    void render(Strip& strip, float t) override
    {
        int n = strip.size();
        if (n <= 0)
            return;

        // whole-pixel distances keep every swing count an integer, so the
        // realignment at the end of the period is exact
        int o = (int)lroundf(origin * (n - 1));
        int far = o > n - 1 - o ? o : n - 1 - o;

        // phase within the period, wrapped so float precision holds up
        // over days of uptime
        float c = fmodf(t, period) / period;

        for (int i = 0; i < n; i++)
        {
            int k = i > o ? i - o : o - i;

            // swings completed so far; only the fraction matters
            float s = c * (swings + k);
            s -= floorf(s);

            // the bob starts at the near end of its swing (lit) and is
            // farthest from it half a swing later
            float v = 0.5f + 0.5f * cosf(2.0f * (float)M_PI * s);
            v = powf(v, sharp);

            float p = (far ? (float)k / far : 0.0f) * span + t * colorSpeed;
            p -= floorf(p);

            uint8_t r, g, b;
            palette.at(p, r, g, b);

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

private:
    color::Gradient palette;
    float period = 60.0f;
    float swings = 15.0f;
    float sharp = 2.5f;
    float span = 1.0f;
    float colorSpeed = 1.0f / 60.0f;
    float origin = 0.0f;
};

REGISTER_EFFECT("pendulum", Pendulum)
