#include <math.h>
#include "effect.hpp"

// CRT-style power-on, the mirror of the shutdown effect: a white-hot point
// ignites at the center, whips outward into a full-width scan line, the
// line resolves from white into the body color, and the strip then holds —
// alive with a faint drifting scanline shimmer — for as long as it runs.
//
// Unlike shutdown, boot never reports finished and never fades to black: it
// owns the strip from power-on until the host daemon connects and sends its
// first frame (which the receiver uses to drop the standalone effect and
// take over). That handoff happens only after limine and the OS have come
// up — seconds after power-on, and not on any fixed schedule — so a timed
// effect would either end early and leave the strip dark through the rest
// of the boot, or overrun. Holding the living glow until the daemon arrives
// sidesteps the timing entirely.
//
// config:
//   intro_seconds  time for the ignite + scan + resolve power-on (default 3)
//   color          body color RRGGBB (default 0028ff)
//   flash_color    hot ignition / scan-line color RRGGBB (default ffffff)
class Boot : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        // accept the legacy duration_seconds as a fallback so existing
        // configs keep a sensible intro pace
        intro = cfg.getFloat("intro_seconds",
                             cfg.getFloat("duration_seconds", 3.0f));
        if (intro <= 0) intro = 3.0f;

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
        int leds = strip.size();
        float center = (leds - 1) * 0.5f;
        float maxRadius = center + 1.0f; // +1 so the end pixels reach full

        // progress through the one-shot intro; clamps at 1 and stays there,
        // dropping into the steady living-glow hold below
        float u = t / intro;
        if (u > 1) u = 1;

        for (int i = 0; i < leds; i++)
            strip.setPixel(i, 0, 0, 0);

        if (u < IGNITE_END)
        {
            // a white-hot point flares up at the center (the gun warming)
            float k = u / IGNITE_END; // 0..1
            float twoSigSq = 2.0f * 0.9f * 0.9f;

            for (int i = 0; i < leds; i++)
            {
                float d = i - center;
                float bright = k * expf(-(d * d) / twoSigSq);
                if (bright <= 0.002f) continue;

                strip.setPixel(i, (uint8_t)(fr * bright),
                               (uint8_t)(fg * bright), (uint8_t)(fb * bright));
            }
        }
        else if (u < SCAN_END)
        {
            // the point whips outward into a full-width bright line: fast
            // out, easing as the fronts reach the ends
            float k = (u - IGNITE_END) / (SCAN_END - IGNITE_END);
            float ce = 1.0f - (1.0f - k) * (1.0f - k);
            float radius = ce * maxRadius;
            if (radius < 0.6f) radius = 0.6f; // keep the center lit through the seam

            for (int i = 0; i < leds; i++)
            {
                // 1-pixel-wide soft edge keeps the scan front from stepping
                float cov = radius - fabsf(i - center) + 0.5f;
                cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
                if (cov <= 0) continue;

                strip.setPixel(i, (uint8_t)(fr * cov),
                               (uint8_t)(fg * cov), (uint8_t)(fb * cov));
            }
        }
        else if (u < RESOLVE_END)
        {
            // the white-hot line resolves into the body color
            float k = (u - SCAN_END) / (RESOLVE_END - SCAN_END);
            uint8_t cr = (uint8_t)(fr + (r - fr) * k);
            uint8_t cg = (uint8_t)(fg + (g - fg) * k);
            uint8_t cb = (uint8_t)(fb + (b - fb) * k);

            for (int i = 0; i < leds; i++)
                strip.setPixel(i, cr, cg, cb);
        }
        else
        {
            // steady state: hold the body color, kept alive by a faint
            // drifting scanline, for as long as the effect runs. No fade —
            // the host's first frame is what ends this (see class comment)
            for (int i = 0; i < leds; i++)
            {
                float wave = 0.88f + 0.12f * sinf(i * 0.6f - t * 5.0f);

                strip.setPixel(i, (uint8_t)(r * wave), (uint8_t)(g * wave),
                               (uint8_t)(b * wave));
            }
        }
    }

    // never finishes: the receiver hands off to the host's first frame
    // whenever the daemon comes up, so boot just holds until then
    bool finished() const override { return false; }

private:
    // phase boundaries as fractions of the intro: ignite, scan out, resolve
    // to body color, then the open-ended living-glow hold
    static constexpr float IGNITE_END = 0.18f;
    static constexpr float SCAN_END = 0.55f;
    static constexpr float RESOLVE_END = 1.0f;

    uint8_t r = 0, g = 40, b = 255;
    uint8_t fr = 255, fg = 255, fb = 255;
    float intro = 3.0f;
};

REGISTER_EFFECT("boot", Boot)
