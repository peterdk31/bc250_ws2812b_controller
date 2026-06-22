#include <math.h>
#include "effect.hpp"

// power-down sequence styled after an old CRT television switching off,
// kept deliberately simple so it reads as one motion at a time: the whole
// strip holds the picture for a beat, the lit line then snap-collapses from
// both ends into the center (body color the entire time — only the width
// changes), there's a single white flash at the center point, and that dot
// fades quietly to black. The collapse is a quick snap; the dying glow is
// what makes it linger, so the strip keeps glowing through the OS power-down
// instead of blanking seconds early. It reports finished once the glow is
// gone so a player can stop and leave the strip dark — unlike boot, this one
// must end (the strip is independently powered and outlives the host, so it
// can't hold a glow forever).
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
            // hold the full picture for a beat, then snap-collapse the lit
            // line inward to the center. Body color stays constant the whole
            // time — only the width changes, so there's one thing to watch.
            float litHalf;
            if (u < HOLD_END)
            {
                litHalf = leds / 2.0f;
            }
            else
            {
                float k = (u - HOLD_END) / (COLLAPSE_END - HOLD_END); // 0..1
                float ce = 1.0f - (1.0f - k) * (1.0f - k);            // fast in, settle
                litHalf = (leds / 2.0f) * (1.0f - ce);
            }

            for (int i = 0; i < leds; i++)
                if (fabsf(i - center) <= litHalf)
                    strip.setPixel(i, r, g, b);
        }
        else
        {
            // a single white dot at the center: it flashes bright, then
            // fades quietly to black with phosphor persistence (fast
            // exponential decay into a faint afterglow). Fixed, tight glow
            // so the dot just dims in place rather than drifting or blooming.
            float k = (u - COLLAPSE_END) / (1.0f - COLLAPSE_END); // 0..1
            float glow = expf(-k * DECAY);                        // 1 -> ~0
            float twoSigSq = 2.0f * SIGMA * SIGMA;

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
    // hold the picture, then a fixed-feeling ~0.5s collapse snap at the
    // default duration; the rest of the run is the lingering phosphor glow
    static constexpr float HOLD_END = 0.12f;     // fraction holding full picture
    static constexpr float COLLAPSE_END = 0.22f; // fraction up to fully collapsed
    static constexpr float DECAY = 3.0f;         // phosphor fade steepness
    static constexpr float SIGMA = 0.9f;         // center-dot tightness (pixels)

    uint8_t r = 0, g = 40, b = 255;
    uint8_t fr = 255, fg = 255, fb = 255;
    float duration = 5.0f;
    float elapsed = 0.0f;
};

REGISTER_EFFECT("shutdown", Shutdown)
