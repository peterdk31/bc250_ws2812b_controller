#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// The default power-off animation (config esp32.shutdown).
//
// Roughly the inverse of button_on: the lit strip "flies" back into the power
// button. The lit region collapses from the farthest point toward the button,
// accelerating as it goes (so it zips into the button at the very end), led by
// a bright head, and the colour fades to white along the way — ending in a
// white flash at the button that goes dark. With the button mid-strip the
// collapse runs inward from both ends at once. It reports finished so the
// receiver can power the strip down after it (like the CRT shutdown).
//
// config:
//   origin            where the button sits along the strip, 0..1 (0 = index-0
//                     end, 1 = last-index end, 0.5 = center); the light
//                     collapses toward here. Defaults to the end picked by
//                     `reverse`
//   reverse           legacy end-picker, used only when `origin` is absent:
//                     false = index-0 end, true = last-index end (default true,
//                     matching button_on)
//   palette           comma-separated stops laid out by distance from the
//                     button (first stop at the button, last at the farthest
//                     pixel) — the starting colours the light fades to white
//                     from as it flies back; match button_on's palette so the
//                     collapse gathers up the same light the power-on spread.
//                     Falls back to `color`
//   color             legacy single starting colour, RRGGBB (default ffffff;
//                     set a colour to see the fade-to-white) — same as a
//                     one-stop palette
//   duration_seconds  collapse time (default 1.4)
class ButtonOff : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        duration = cfg.getFloat("duration_seconds", 1.4f);
        if (duration <= 0) duration = 1.4f;

        bool reverse = cfg.getBool("reverse", true);
        origin = cfg.getFloat("origin", reverse ? 1.0f : 0.0f);
        if (origin < 0) origin = 0;
        if (origin > 1) origin = 1;

        palette = color::Gradient(cfg.get("palette", cfg.get("color", "ffffff")));
    }

    void render(Strip& strip, float t) override
    {
        elapsed = t;

        int leds = strip.size();

        // button-distance space, as in button_on: p is a pixel's distance
        // from the button, maxDist the farthest pixel's — a mid-strip button
        // collapses both halves symmetrically. The feather scales with the
        // travel distance so the edge stays proportionally soft.
        float originLed = origin * (leds - 1);
        float maxDist = fmaxf(originLed, (leds - 1) - originLed);
        float feather = (maxDist + 1.0f) * 0.18f + 1.0f;

        float u = t / duration;
        if (u > 1) u = 1;

        // the lit region's far edge collapses toward the button; (1-u*u)
        // accelerates the collapse so it zips into the button near the end
        float edge = (maxDist + feather) * (1.0f - u * u);

        // the colours fade to white as the light flies back
        float w = motion::ease(u);

        // ease the whole thing to black over the last fifth so it ends dark
        float endFade = u > 0.8f ? 1.0f - (u - 0.8f) / 0.2f : 1.0f;
        if (endFade < 0) endFade = 0;

        for (int i = 0; i < leds; i++)
        {
            float p = fabsf(i - originLed);
            float x = maxDist > 0 ? p / maxDist : 0.0f;

            // lit between the button and the collapsing edge, dark beyond it
            float cov = motion::ease((edge - p) / feather + 0.5f);

            // bright head riding the collapsing edge — the gathered light
            float dh = p - edge;
            float head = 0.7f * expf(-(dh * dh) / (2.0f * 1.6f * 1.6f));

            float v = cov * 0.8f + head;
            if (v > 1) v = 1;
            v *= endFade * (0.9f + 0.1f * motion::shimmer(x, t));

            // the pixel's palette colour (by distance from the button, laid
            // out as in button_on), whitening as the collapse progresses
            uint8_t pr, pg, pb;
            palette.at(x, pr, pg, pb);

            float cr = pr + (255 - pr) * w;
            float cg = pg + (255 - pg) * w;
            float cb = pb + (255 - pb) * w;

            strip.setPixel(i, (uint8_t)(cr * v), (uint8_t)(cg * v),
                           (uint8_t)(cb * v));
        }
    }

    bool finished() const override { return elapsed >= duration; }

private:
    color::Gradient palette;
    float duration = 1.4f;
    float elapsed = 0.0f;
    float origin = 1.0f;
};

REGISTER_EFFECT("button_off", ButtonOff)
