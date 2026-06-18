#include <math.h>
#include "effect.hpp"

// power-down sequence styled after an old CRT television switching off:
// the whole strip is lit, then the picture collapses fast from both ends
// into a single white-hot point at the center (the electron beam focusing
// down — the shrinking core gets brighter, not dimmer), which blooms and
// then fades out slowly with phosphor persistence. The collapse is a quick
// snap; the long dying glow is what makes it linger, so the strip keeps
// glowing through the OS power-down instead of blanking seconds early. It
// reports finished once the glow is gone so a player can stop and leave the
// strip dark — unlike boot, this one must end (the strip is independently
// powered and outlives the host, so it can't hold a glow forever).
//
// self-contained (it doesn't depend on whatever was on the strip), so it
// looks the same whether the daemon plays it on SIGTERM or the receiver
// plays it on a shutdown command after the host has gone.
//
// config:
//   duration_seconds  total run time (default 5.0)
//   color             body color RRGGBB while the picture is up (default 0028ff)
//   flash_color       hot collapse / phosphor-dot color RRGGBB (default ffffff)
class Shutdown : public Effect
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

        uint32_t flash = cfg.getColor("flash_color", 0xffffff);
        fr = (flash >> 16) & 0xFF;
        fg = (flash >> 8) & 0xFF;
        fb = flash & 0xFF;
    }

    void render(Strip& strip, float t) override
    {
        elapsed = t;

        int leds = strip.size();
        float u = t / duration;
        if (u > 1) u = 1;

        float center = (leds - 1) / 2.0f;

        for (int i = 0; i < leds; i++)
            strip.setPixel(i, 0, 0, 0);

        if (u < COLLAPSE_END)
        {
            // the picture collapses inward to a point: the lit half-width
            // races to zero (fast in, easing as it lands) while the color
            // surges from the body color to the hot beam color, so the
            // shrinking core reads as energy concentrating, not dying.
            float k = u / COLLAPSE_END;                  // 0..1
            float ce = 1.0f - (1.0f - k) * (1.0f - k);   // fast start, settle
            float litHalf = (leds / 2.0f) * (1.0f - ce);

            uint8_t cr = (uint8_t)(r + (fr - r) * ce);
            uint8_t cg = (uint8_t)(g + (fg - g) * ce);
            uint8_t cb = (uint8_t)(b + (fb - b) * ce);

            for (int i = 0; i < leds; i++)
            {
                float d = fabsf(i - center);

                if (d <= litHalf)
                    strip.setPixel(i, cr, cg, cb);
                else if (d <= litHalf + 1.0f)
                    strip.setPixel(i, fr, fg, fb); // bright collapsing edge
            }
        }
        else
        {
            // the white-hot dot blooms, then fades with phosphor
            // persistence: a fast exponential decay trailing into a long
            // faint afterglow. It narrows toward a single pixel as it dies.
            float k = (u - COLLAPSE_END) / (1.0f - COLLAPSE_END); // 0..1
            float glow = expf(-k * DECAY);                         // 1 -> ~0
            float sigma = 0.6f + 1.8f * glow;                      // tightens as it fades
            float twoSigSq = 2.0f * sigma * sigma;

            for (int i = 0; i < leds; i++)
            {
                float d = fabsf(i - center);
                float bright = glow * expf(-(d * d) / twoSigSq);
                if (bright <= 0.002f) continue;

                strip.setPixel(i, (uint8_t)(fr * bright),
                               (uint8_t)(fg * bright), (uint8_t)(fb * bright));
            }
        }
    }

    bool finished() const override { return elapsed >= duration; }

private:
    // collapse stays a fixed-feeling ~0.5s snap at the default duration; the
    // rest of the (now longer) run is the lingering phosphor afterglow
    static constexpr float COLLAPSE_END = 0.1f; // fraction spent collapsing
    static constexpr float DECAY = 3.0f;        // phosphor fade steepness

    uint8_t r = 0, g = 40, b = 255;
    uint8_t fr = 255, fg = 255, fb = 255;
    float duration = 5.0f;
    float elapsed = 0.0f;
};

REGISTER_EFFECT("shutdown", Shutdown)
