#include <math.h>
#include "effect.hpp"
#include "steam.hpp"

// progress bar for Steam downloads (steam.hpp), activated by the
// steam_dl condition. The head pixel is antialiased, so even a 10-LED
// strip resolves single percents; the displayed value glides toward
// the target so coarse updates animate instead of jumping. Completed
// LEDs sit steady at full, the head LED holds the fractional percent,
// and once per flow_period a brightness crest sweeps start -> head and
// then pauses, so the bar reads as a clean fill with a live pulse
// rather than a constant ripple. The unfilled remainder glows as a
// faint track to show the mode is active, and at 100 the whole bar
// pulses in done_color until the rule deactivates. While the download
// is inactive (paused, gone) the last value holds — the rule engine
// switches away shortly after
//
// config:
//   color              bar RRGGBB (default 00a0ff)
//   done_color         finished pulse RRGGBB (default 00ff40)
//   smoothing_seconds  glide time constant (default 0.4)
//   flow_period        seconds between flow pulses (default 2.5)
//   flow_depth         how far the bar dims away from the sweeping
//                      crest, 0..1 (default 0.35; 0 disables the flow)
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

        flowPeriod = cfg.getFloat("flow_period", 2.5f);
        flowDepth = cfg.getFloat("flow_depth", 0.35f);

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

        // a single crest sweeps start -> head over the first part of each
        // period, then the bar rests at full until the next sweep, so the
        // fill reads cleanly between pulses. env fades the dimming in and
        // out across the sweep so there is no jump at the period boundary.
        const float sweep_frac = 0.65f; // share of the period spent sweeping
        const float crest_width = 1.2f;  // crest half-width, in LEDs

        float env = 0.0f, crest = 0.0f;

        if (flowDepth > 0 && flowPeriod > 0)
        {
            float cyc = fmodf(t, flowPeriod) / flowPeriod;

            if (cyc < sweep_frac)
            {
                float sp = cyc / sweep_frac; // 0..1 across the sweep
                env = sinf((float)M_PI * sp); // 0 -> 1 -> 0
                crest = sp * pos;             // travels to the head
            }
        }

        for (int i = 0; i < leds; i++)
        {
            float v = i < head ? 1.0f
                    : i == head ? frac
                    : 0.0f;

            // completed LEDs stay at full; the sweeping crest only dims
            // the bar away from itself while a pulse is passing
            if (i <= head && env > 0)
            {
                float d = (i + 0.5f) - crest;
                float bump = expf(-(d * d) / (2.0f * crest_width * crest_width));
                v *= 1.0f - env * flowDepth * (1.0f - bump);
            }

            // floor the unfilled track to a faint glow so the bar's
            // extent (and that the mode is active) still reads when the
            // download sits near 0%. This has to clear the host's
            // gamma/brightness lut: at the default gamma 2.2 and 20%
            // brightness the gamma curve crushes small values hard, so a
            // 0.15 floor mapped to a single unit of blue — invisible, and
            // an empty bar read as a dead-black strip. 0.30 survives as a
            // dim blue (~8% of the filled bar) with room to spare.
            if (v < 0.30f)
                v = 0.30f;

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

    int frameDelayMs() const override { return 33; }

private:
    uint8_t r = 0, g = 160, b = 255;
    uint8_t dr = 0, dg = 255, db = 64;
    float smoothing = 0.4f;
    float flowPeriod = 2.5f;
    float flowDepth = 0.35f;
    float target = 0.0f;
    float shown = 0.0f;
};

REGISTER_EFFECT("steam_download", SteamDownload)
