#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// waves running up a shore: soft crests are born at each end of the strip
// and roll inward, decelerating like water running up sand, whitening into
// foam as they expire near the center — where this case's power button
// sits, so the waves visibly break around it. Pulse played backwards, from
// both ends at once. With the default half-period stagger the two shores
// alternate; stagger 0 launches them together so mirrored crests meet and
// break at the center simultaneously. Deterministic, no randomness.
//
// config:
//   palette         crest stops sampled by intensity, soft fringe -> bright
//                   crest (default "003048,00a090,60e0c0",
//                   deep water -> teal -> aqua)
//   foam_color      RRGGBB the crest whitens toward as it expires
//                   (default ffffff)
//   base_color      still-water background RRGGBB (default 000810)
//   period_seconds  seconds between waves per shore (default 6)
//   width           crest half-width as a fraction of the strip
//                   (default 0.09)
//   travel          how many periods a wave takes to reach the center
//                   (default 1.6, so ~2 waves ride each shore at once)
//   stagger         launch offset of the far shore in periods, 0..1
//                   (default 0.5 — alternating; 0 = mirrored pairs)
class Shoreline : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "003048,00a090,60e0c0"));
        unpack(cfg.getColor("base_color", 0x000810), base);
        unpack(cfg.getColor("foam_color", 0xffffff), foam);

        period = cfg.getFloat("period_seconds", 6.0f);
        if (period <= 0.5f) period = 0.5f;
        width = cfg.getFloat("width", 0.09f);
        if (width < 0.02f) width = 0.02f;
        travel = cfg.getFloat("travel", 1.6f);
        if (travel < 0.2f) travel = 0.2f;
        stagger = cfg.getFloat("stagger", 0.5f);
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();
        float inv2w2 = 1.0f / (2.0f * width * width);
        float travelT = period * travel;
        int waves = (int)ceilf(travel) + 1;

        for (int i = 0; i < leds; i++)
        {
            float x = leds > 1 ? (float)i / (leds - 1) : 0.0f;

            // sum the crests riding in from both shores; track how much of
            // the light is foam so the color whitens where waves expire
            float light = 0.0f, foamW = 0.0f;

            for (int s = 0; s < 2; s++)
            {
                float ts = t - (s ? stagger * period : 0.0f);
                int newest = (int)floorf(ts / period);

                for (int k = 0; k <= waves; k++)
                {
                    float age = ts - (newest - k) * period;
                    float u = age / travelT;
                    if (u < 0.0f || u >= 1.0f) continue;

                    // sin ramp: the crest starts quick off the shore and
                    // decelerates into the center like water up sand
                    float prog = sinf(1.5707963f * u);
                    float pos = s ? 1.0f - 0.5f * prog : 0.5f * prog;

                    // swell in from nothing, die away at the center
                    float amp = sinf(3.14159265f * u);

                    float d = x - pos;
                    float w = amp * expf(-d * d * inv2w2);

                    light += w;
                    foamW += w * u * u * u;   // whiten late in the run
                }
            }

            // foam fraction: the amplitude-weighted mean of u^3 — how late
            // in their run the waves lighting this pixel are (use the sum
            // before clamping so the ratio stays a true mean)
            float fw = light > 0.0f ? foamW / light : 0.0f;
            if (light > 1.0f) light = 1.0f;

            // hue follows intensity, then leans toward foam where the wave
            // is breaking, then blends down into the still water
            uint8_t gr, gg, gb;
            palette.at(light, gr, gg, gb);

            float cr = gr + (foam[0] - gr) * fw;
            float cg = gg + (foam[1] - gg) * fw;
            float cb = gb + (foam[2] - gb) * fw;

            strip.setPixel(i, mix(base[0], cr, light),
                           mix(base[1], cg, light),
                           mix(base[2], cb, light));
        }
    }

private:
    color::Gradient palette;
    float base[3] = {0, 8, 16};
    float foam[3] = {255, 255, 255};
    float period = 6.0f;
    float width = 0.09f;
    float travel = 1.6f;
    float stagger = 0.5f;

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

REGISTER_EFFECT("shoreline", Shoreline)
