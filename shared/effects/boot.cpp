#include <math.h>
#include "effect.hpp"

// CRT-style power-on, the mirror of the shutdown effect: a white-hot point
// ignites at the center, whips outward into a full-width scan line, the
// line resolves from white into the body color, the strip then holds —
// alive with a faint drifting scanline shimmer — and finally fades to
// black, handing a dark strip to the rule engine. Reports finished so the
// playlist advances. The ignition and scan are fast; the lit hold is what
// makes it last.
//
// config:
//   duration_seconds  total run time (default 6)
//   color             body color RRGGBB (default 0028ff)
//   flash_color       hot ignition / scan-line color RRGGBB (default ffffff)
class Boot : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        duration = cfg.getFloat("duration_seconds", 6.0f);
        if (duration <= 0) duration = 6.0f;

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
        float center = (leds - 1) * 0.5f;
        float maxRadius = center + 1.0f; // +1 so the end pixels reach full

        float u = t / duration;
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
            // hold the body color, kept alive by a faint drifting
            // scanline, then fade to black over the tail
            float dim = 1.0f;
            if (u >= FADE_START)
            {
                // quadratic fade: quick drop, soft landing into black
                float k = 1.0f - (u - FADE_START) / (1.0f - FADE_START);
                dim = k * k;
            }

            for (int i = 0; i < leds; i++)
            {
                float wave = 0.88f + 0.12f * sinf(i * 0.6f - t * 5.0f);
                float v = dim * wave;

                strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                               (uint8_t)(b * v));
            }
        }
    }

    bool finished() const override { return elapsed >= duration; }

private:
    // phase boundaries as fractions of the total duration: ignite, scan
    // out, resolve to body color, then a long lit hold ending in a fade
    static constexpr float IGNITE_END = 0.04f;
    static constexpr float SCAN_END = 0.12f;
    static constexpr float RESOLVE_END = 0.22f;
    static constexpr float FADE_START = 0.82f;

    uint8_t r = 0, g = 40, b = 255;
    uint8_t fr = 255, fg = 255, fb = 255;
    float duration = 6.0f;
    float elapsed = 0.0f;
};

REGISTER_EFFECT("boot", Boot)
