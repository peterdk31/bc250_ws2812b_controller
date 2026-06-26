#include <math.h>
#include "effect.hpp"
#include "color.hpp"

// soft rings of light born at the center on a slow period, swelling as they
// expand outward and fading as they reach the ends — a calm, meditative
// throb. Several rings can be in flight at once so they overlap gently.
// Deterministic, no randomness.
//
// config:
//   palette         ring stops sampled by ring intensity, so the soft halo
//                   edge and the bright core differ in hue (default
//                   "5028ff,30d0ff", violet -> cyan). Use a single stop for a
//                   solid ring color, e.g. "30c0ff".
//   base_color      background RRGGBB (default 000010)
//   period_seconds  seconds between rings (default 4)
//   width           ring half-width as a fraction of the half-strip
//                   (default 0.18)
//   travel          how many periods a ring takes to fade out (default 1.4,
//                   so up to ~2 rings are visible at once)
class Pulse : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "5028ff,30d0ff"));
        unpack(cfg.getColor("base_color", 0x000010), base);

        period = cfg.getFloat("period_seconds", 4.0f);
        if (period <= 0) period = 4.0f;

        width = cfg.getFloat("width", 0.18f);
        if (width < 0.02f) width = 0.02f;

        travel = cfg.getFloat("travel", 1.4f);
        if (travel < 0.2f) travel = 0.2f;
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();
        float center = (leds - 1) / 2.0f;
        float inv2sigma2 = 1.0f / (2.0f * width * width);
        float travelT = period * travel;

        // ring launches happen at integer multiples of the period; sum the
        // few most recent that haven't yet faded
        int newest = (int)floorf(t / period);
        int rings = (int)ceilf(travel) + 1;

        for (int i = 0; i < leds; i++)
        {
            float dist = center > 0 ? fabsf(i - center) / center : 0.0f;

            float light = 0.0f;

            for (int k = 0; k <= rings; k++)
            {
                float age = t - (newest - k) * period;     // >= 0
                float radius = age / travelT;               // 0 .. 1
                if (radius >= 1.0f) continue;               // faded out

                // a half-sine envelope over the ring's life: zero at birth so
                // it fades in from the center instead of popping on at full
                // brightness, peaks mid-flight, then fades back to zero at the
                // rim. inv: linear `1 - radius` snapped a fresh ring on hard.
                float amp = sinf(3.14159265f * radius);
                if (amp <= 0) continue;

                float d = dist - radius;
                light += amp * expf(-d * d * inv2sigma2);
            }

            if (light > 1.0f) light = 1.0f;

            // hue follows intensity: faint halo edge -> first stop, bright
            // core -> last stop, then blended down into the base by `light`
            uint8_t gr, gg, gb;
            palette.at(light, gr, gg, gb);

            strip.setPixel(i, mix(base[0], gr, light),
                           mix(base[1], gg, light),
                           mix(base[2], gb, light));
        }
    }

private:
    color::Gradient palette;
    float base[3] = {0, 0, 16};
    float period = 4.0f;
    float width = 0.18f;
    float travel = 1.4f;

    static void unpack(uint32_t c, float out[3])
    {
        out[0] = (c >> 16) & 0xFF;
        out[1] = (c >> 8) & 0xFF;
        out[2] = c & 0xFF;
    }

    static uint8_t mix(float a, float b, float w)
    {
        return (uint8_t)(a + (b - a) * w);
    }
};

REGISTER_EFFECT("pulse", Pulse)
