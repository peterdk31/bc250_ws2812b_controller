#include <math.h>
#include "effect.hpp"
#include "motion.hpp"

// Power-on bloom: a calm, edge-free open. A faint seed glows at the centre of
// the strip, then ignites and blooms outward into a full wash of the body
// colour. There is no moving scan front and no white-hot flash (the old CRT
// boot had both, which is what made it feel electronic) — the radius grows
// behind a wide, permanently feathered edge, and crucially eases *out* at the
// top so the bloom decelerates into the ends instead of whipping open.
//
// After the bloom it exhales: brightness eases back down to settle_level over
// settle_seconds and then holds there. That's so the recording can hand off
// to a looping idle segment (breathe) seamlessly — breathe opens at its dim
// trough, and the exhale decelerates into that same level, so the bloom reads
// as one big inhale leading straight into the idle's breathing. Set
// settle_level to match the idle's min_brightness. It never reports finished
// (the idle segment, or the host daemon's first frame, is what follows).
//
// config:
//   intro_seconds  bloom time (default 7 — a slow, calm open)
//   color          body colour RRGGBB (default ffffff, white)
//   settle_seconds exhale time after the bloom (default 1.5)
//   settle_level   brightness the exhale lands and holds at (default 0.05;
//                  match the idle segment's min_brightness)
class Boot : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        // accept legacy duration_seconds as a fallback for older configs
        intro = cfg.getFloat("intro_seconds",
                             cfg.getFloat("duration_seconds", 7.0f));
        if (intro <= 0) intro = 7.0f;

        settle = cfg.getFloat("settle_seconds", 1.5f);
        if (settle <= 0) settle = 1.5f;
        settleLevel = cfg.getFloat("settle_level", 0.05f);

        uint32_t color = cfg.getColor("color", 0xffffff);
        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();
        float center = (leds - 1) * 0.5f;

        // a soft edge a quarter of the strip wide
        float feather = leds * 0.25f + 1.0f;

        // bloom progress, clamped so the radius stays full once it has opened
        float u = t / intro;
        float ub = u < 1 ? u : 1;

        // slow "ignition": squaring ub before easing makes the radius linger
        // as a faint seed at the centre through the first part of the intro,
        // then open out. Because ease() still flattens at the top the bloom
        // decelerates into the ends rather than whipping open (the whip is
        // what made the old boot read as a CRT).
        float radius = motion::ease(ub * ub) * (center + feather);

        // brightness envelope: ramp in off a small floor during the bloom so
        // the seed glows up, then exhale back down to settle_level and hold —
        // ease() at both turns gives zero slope at the top of the bloom and at
        // the bottom of the exhale, so it joins the idle's trough smoothly.
        float level;
        if (u < 1)
            level = 0.25f + 0.75f * motion::ease(u);
        else
        {
            float s = (t - intro) / settle;
            if (s > 1) s = 1;
            level = 1.0f - (1.0f - settleLevel) * motion::ease(s);
        }

        for (int i = 0; i < leds; i++)
        {
            float x = leds > 1 ? (float)i / (leds - 1) : 0.0f;
            float d = fabsf(i - center);

            // feathered coverage: 1 well inside the radius, smoothly to 0
            // across the soft edge — no hard front anywhere
            float cov = motion::ease((radius - d) / feather + 0.5f);

            float v = cov * level * (0.92f + 0.08f * motion::shimmer(x, t));

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

    // never finishes on its own; the recording's idle segment (or the host's
    // first frame) is what follows the hold
    bool finished() const override { return false; }

private:
    uint8_t r = 255, g = 255, b = 255;
    float intro = 7.0f;
    float settle = 1.5f;
    float settleLevel = 0.05f;
};

REGISTER_EFFECT("boot", Boot)
