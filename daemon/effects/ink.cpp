#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// drops of ink: every period a drop of the next palette color is born
// somewhere along the strip and diffuses outward as a soft eased front,
// dyeing the whole strip over the previous color. The strip is always fully
// lit in *some* color — what changes is its whole identity, recolored in
// slow sweeping waves. The advancing front carries a faint glistening rim
// so a dye pass reads even between similar colors, and a gentle shimmer
// keeps the settled color breathing between drops. Deterministic: drop
// times, places and colors are all pure functions of the drop index.
//
// config:
//   palette         stops the drops walk through; successive drops take
//                   well-separated points of it (a golden-ratio stride), so
//                   neighbours always contrast (default
//                   "6a00ff,0080ff,00d0a0,ffb000,ff2060")
//   period_seconds  seconds between drops (default 10)
//   spread_seconds  how long a front takes to cover the strip (default 4);
//                   capped at 2*period so at most a few fronts overlap
//   softness        front half-width as a fraction of the strip
//                   (default 0.08)
//   rim             strength of the glistening leading edge, 0 disables
//                   (default 0.35)
//   rim_color       RRGGBB the rim leans toward (default ffffff)
//   sheen           brightness depth of the idle shimmer, 0..1
//                   (default 0.15); the floor never drops below 1 - sheen
class Ink : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette",
                                          "6a00ff,0080ff,00d0a0,ffb000,ff2060"));
        period = cfg.getFloat("period_seconds", 10.0f);
        if (period <= 0.5f) period = 0.5f;
        spread = cfg.getFloat("spread_seconds", 4.0f);
        if (spread <= 0.2f) spread = 0.2f;
        if (spread > 2.0f * period) spread = 2.0f * period;
        soft = cfg.getFloat("softness", 0.08f);
        if (soft < 0.02f) soft = 0.02f;
        rim = cfg.getFloat("rim", 0.35f);
        if (rim < 0.0f) rim = 0.0f;
        sheen = cfg.getFloat("sheen", 0.15f);
        if (sheen < 0.0f) sheen = 0.0f;
        if (sheen > 1.0f) sheen = 1.0f;

        uint32_t rc = cfg.getColor("rim_color", 0xffffff);
        rimR = (rc >> 16) & 0xFF;
        rimG = (rc >> 8) & 0xFF;
        rimB = rc & 0xFF;
    }

    void render(Strip& strip, float t) override
    {
        // drops still in flight: the newest few whose fronts haven't
        // finished. Everything older is fully spread, so the oldest
        // considered drop is the settled base color.
        int newest = (int)floorf(t / period);
        int live = (int)ceilf(spread / period) + 1;

        for (int i = 0; i < strip.size(); i++)
        {
            float x = strip.size() > 1
                ? (float)i / (strip.size() - 1) : 0.0f;

            uint8_t r, g, b;
            dropColor(newest - live, r, g, b);
            float cr = r, cg = g, cb = b;

            float glisten = 0.0f;

            for (int j = newest - live + 1; j <= newest; j++)
            {
                float age = t - j * period;
                if (age < 0.0f) continue;

                float u = age / spread;
                if (u > 1.0f) u = 1.0f;

                // the front decelerates as it runs out — ease'd radius,
                // stretched past the far edge so coverage completes
                float pj = dropPos(j);
                float maxR = (pj > 0.5f ? pj : 1.0f - pj) + 2.0f * soft;
                float radius = motion::ease(u) * maxR;

                float d = fabsf(x - pj);

                // coverage: 1 behind the front, 0 ahead, eased across it
                float c = (radius - d) / soft;
                if (c < 0.0f) c = 0.0f;
                if (c > 1.0f) c = 1.0f;
                c = motion::ease(c);

                dropColor(j, r, g, b);
                cr += (r - cr) * c;
                cg += (g - cg) * c;
                cb += (b - cb) * c;

                // glistening rim on the leading edge, fading out as the
                // dye completes
                if (u < 1.0f && rim > 0.0f)
                {
                    float e = (d - radius) / soft;
                    glisten += rim * expf(-e * e * 2.0f)
                             * sinf(3.14159265f * u);
                }
            }

            if (glisten > 1.0f) glisten = 1.0f;

            // the rim leans the color toward rim_color and lifts the
            // brightness a touch; the settled color shimmers gently
            cr += (rimR - cr) * glisten * 0.7f;
            cg += (rimG - cg) * glisten * 0.7f;
            cb += (rimB - cb) * glisten * 0.7f;

            float v = 1.0f - sheen + sheen * motion::shimmer(x, t, 1.0f);
            v *= 1.0f + 0.3f * glisten;
            if (v > 1.0f) v = 1.0f;

            strip.setPixel(i, (uint8_t)(cr * v), (uint8_t)(cg * v),
                           (uint8_t)(cb * v));
        }
    }

private:
    // drop j's color: a golden-ratio stride through the palette, so
    // successive drops land far apart and neighbours always contrast
    void dropColor(int j, uint8_t& r, uint8_t& g, uint8_t& b) const
    {
        float p = j * 0.381966f;
        p -= floorf(p);
        palette.at(p, r, g, b);
    }

    // drop j's birth point: a per-drop hash, kept off the extreme ends so
    // every front visibly travels both ways
    static float dropPos(int j)
    {
        return 0.15f + 0.7f * motion::hash2(j, 91);
    }

    color::Gradient palette;
    float period = 10.0f;
    float spread = 4.0f;
    float soft = 0.08f;
    float rim = 0.35f;
    float sheen = 0.15f;
    float rimR = 255, rimG = 255, rimB = 255;
};

REGISTER_EFFECT("ink", Ink)
