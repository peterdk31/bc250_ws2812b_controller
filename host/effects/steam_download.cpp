#include <math.h>
#include "effect.hpp"
#include "steam.hpp"

// progress bar for Steam downloads (steam.hpp), activated by the
// steam_dl condition. The head pixel is antialiased, so even a 10-LED
// strip resolves single percents; the displayed value glides toward
// the target so coarse updates animate instead of jumping. The
// unfilled remainder glows as a faint track to show the mode is
// active, and at 100 the whole bar pulses in done_color until the
// rule deactivates. While the download is inactive (paused, gone) the
// last value holds — the rule engine switches away shortly after
//
// config:
//   color              bar RRGGBB (default 00a0ff)
//   done_color         finished pulse RRGGBB (default 00ff40)
//   smoothing_seconds  glide time constant (default 0.4)
class SteamDownload : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        uint32_t color = cfg.getColor("color", 0x00a0ff);

        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;

        uint32_t done = cfg.getColor("done_color", 0x00ff40);

        dr = (done >> 16) & 0xFF;
        dg = (done >> 8) & 0xFF;
        db = done & 0xFF;

        smoothing = cfg.getFloat("smoothing_seconds", 0.4f);

        // seed from the live percentage rather than 0: while a game
        // downloads, proc:steam / cpu_load / gpu_load rules contend for
        // the strip, so this effect is re-activated (and re-init'd) on
        // every bit of churn. Starting at 0 each time left the bar
        // forever gliding up from empty and reading blank; resuming at
        // the current percent makes a re-activation seamless.
        const steam::Downloads& dl = steam::downloads();

        target = dl.percent / 100.0f;
        shown = target;
    }

    void render(WS2812Serial& strip, float t) override
    {
        const steam::Downloads& dl = steam::downloads();

        if (dl.active)
            target = dl.percent / 100.0f;

        float dt = frameDelayMs() / 1000.0f;
        float alpha = smoothing > 0 ? 1.0f - expf(-dt / smoothing) : 1.0f;

        shown += alpha * (target - shown);

        int leds = strip.size();

        if (shown >= 0.995f)
        {
            // done: slow full-strip pulse until the rule deactivates
            float v = 0.6f + 0.4f * sinf(t * 2.0f * (float)M_PI / 1.5f);

            for (int i = 0; i < leds; i++)
                strip.setPixel(i, (uint8_t)(dr * v), (uint8_t)(dg * v),
                               (uint8_t)(db * v));

            return;
        }

        float pos = shown * leds;
        int head = (int)pos;
        float frac = pos - head;

        for (int i = 0; i < leds; i++)
        {
            // 0.15 is the faintest level that survives the host's
            // gamma/brightness lut at the default 20% brightness
            float v = i < head ? 1.0f
                    : i == head ? frac
                    : 0.0f;

            if (v < 0.15f)
                v = 0.15f;

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

    int frameDelayMs() const override { return 33; }

private:
    uint8_t r = 0, g = 160, b = 255;
    uint8_t dr = 0, dg = 255, db = 64;
    float smoothing = 0.4f;
    float target = 0.0f;
    float shown = 0.0f;
};

REGISTER_EFFECT("steam_download", SteamDownload)
