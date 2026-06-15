#include <math.h>
#include "effect.hpp"

// power-down sequence — the boot effect in reverse: the strip lights in
// the body color, pulses once toward the flash color, then darkness eats
// inward from both ends to the center until the strip is black. It then
// reports finished so a player can stop and leave the strip dark.
//
// self-contained (it doesn't depend on whatever was on the strip), so it
// looks the same whether the daemon plays it on SIGTERM or the receiver
// plays it on a shutdown command after the host has gone.
//
// config:
//   duration_seconds  total run time (default 1.5)
//   color             body color RRGGBB (default 0028ff)
//   flash_color       pulse / collapsing-edge color RRGGBB (default ffffff)
class Shutdown : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        duration = cfg.getFloat("duration_seconds", 1.5f);
        if (duration <= 0) duration = 1.5f;

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

        // body color, surging to the flash color during the opening pulse
        uint8_t br = r, bg = g, bb = b;

        if (u < PULSE_END)
        {
            float k = u / PULSE_END;
            br = (uint8_t)(r + (fr - r) * k);
            bg = (uint8_t)(g + (fg - g) * k);
            bb = (uint8_t)(b + (fb - b) * k);
        }

        // after the pulse, darkness closes in from both ends; the
        // surviving core shrinks and dims, smoothstepped so it starts
        // gently and snaps shut at the end
        float collapse = u < PULSE_END
                             ? 0.0f
                             : (u - PULSE_END) / (1.0f - PULSE_END);
        float ce = collapse * collapse * (3.0f - 2.0f * collapse);

        float center = (leds - 1) / 2.0f;
        float litHalf = (leds / 2.0f) * (1.0f - ce);
        float dim = 1.0f - ce;

        for (int i = 0; i < leds; i++)
        {
            float d = fabsf(i - center);

            if (d <= litHalf)
                strip.setPixel(i, (uint8_t)(br * dim),
                               (uint8_t)(bg * dim), (uint8_t)(bb * dim));
            else if (d <= litHalf + 1.0f)
                strip.setPixel(i, fr, fg, fb); // bright collapsing edge
            else
                strip.setPixel(i, 0, 0, 0);
        }
    }

    bool finished() const override { return elapsed >= duration; }

private:
    static constexpr float PULSE_END = 0.20f; // fraction spent on the pulse

    uint8_t r = 0, g = 40, b = 255;
    uint8_t fr = 255, fg = 255, fb = 255;
    float duration = 1.5f;
    float elapsed = 0.0f;
};

REGISTER_EFFECT("shutdown", Shutdown)
