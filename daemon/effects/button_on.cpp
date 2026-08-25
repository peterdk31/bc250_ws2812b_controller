#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// The default power-on animation (config esp32.power_on).
//
// The idea: the strip's light looks like it spills out of the physical power
// button, which may sit past either end of the strip or anywhere along it
// (see `origin`). A bright front bursts fast out of the button
// and then spreads more slowly along the strip — an ease-out front: high speed
// at the button, decelerating as it reaches the far end — led by a soft glowing
// head so it reads as light being pushed in rather than a wipe. Once the strip
// is full it settles into a slow pulse that travels outward from the
// button end, and never ends: it holds until the host daemon's first frame
// takes over.
//
// config:
//   origin          where the button sits along the strip, 0..1 (0 = index-0
//                   end, 1 = last-index end, 0.5 = center). The light radiates
//                   outward from here — from the center that's two symmetric
//                   fronts. Defaults to the end picked by `reverse`
//   reverse         legacy end-picker, used only when `origin` is absent:
//                   false = index-0 end, true = last-index end (default true)
//   palette         comma-separated stops laid out by distance from the
//                   button — first stop at the button, last at the farthest
//                   pixel — so a rainbow palette expands as bands of colour
//                   radiating out of the button. The glowing head stays
//                   white-hot on top. Falls back to `color`
//   color           legacy single colour, RRGGBB (default ffffff) — same as
//                   a one-stop palette
//   spread_seconds  time for the light to fill the strip (default 2.5)
//   period_seconds  pulse period once filled (default 4)
//   min_brightness  pulse floor 0..1 (default 0.25)
class ButtonOn : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        spread = cfg.getFloat("spread_seconds", 2.5f);
        if (spread <= 0) spread = 2.5f;

        period = cfg.getFloat("period_seconds", 4.0f);
        if (period <= 0) period = 4.0f;

        minLevel = cfg.getFloat("min_brightness", 0.25f);

        bool reverse = cfg.getBool("reverse", true);
        origin = cfg.getFloat("origin", reverse ? 1.0f : 0.0f);
        if (origin < 0) origin = 0;
        if (origin > 1) origin = 1;

        palette = color::Gradient(cfg.get("palette", cfg.get("color", "ffffff")));
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();

        // everything below runs in button-distance space: p is a pixel's
        // distance from the button, maxDist the farthest pixel's. With the
        // button at an end that's the whole strip; from the center it's two
        // symmetric halves filled by the same front. The feather scales with
        // the travel distance so the front stays proportionally soft.
        float originLed = origin * (leds - 1);
        float maxDist = fmaxf(originLed, (leds - 1) - originLed);
        float feather = (maxDist + 1.0f) * 0.18f + 1.0f;

        if (t < spread)
        {
            // ease-out front: fast out of the button, slowing as it spreads.
            // 1-(1-u)^p has its highest speed at u=0 and decelerates to a stop.
            // The travel ends at (farthest pixel + half a feather), which is
            // where that pixel reaches full coverage — so the strip finishes
            // filling exactly at t=spread, with no static hold before the pulse.
            float u = t / spread;
            float fp = 1.0f - powf(1.0f - u, 2.5f);
            float front = fp * (maxDist + 0.5f * feather);

            for (int i = 0; i < leds; i++)
            {
                float p = fabsf(i - originLed);
                float x = maxDist > 0 ? p / maxDist : 0.0f;

                // white fill behind the feathered front
                float cov = motion::ease((front - p) / feather + 0.5f);

                // a soft bright head that glows a touch ahead of the front,
                // so the light looks like it's being pushed into the strip
                float dh = p - front;
                float head = 0.55f * expf(-(dh * dh) / (2.0f * 1.6f * 1.6f));

                float v = cov * (0.95f + 0.05f * motion::shimmer(x, t));

                // palette colour by distance from the button; the head adds
                // white on top so the leading edge reads hot whatever the
                // local colour (for a white palette this reduces to the
                // plain brightness sum)
                uint8_t cr, cg, cb;
                palette.at(x, cr, cg, cb);

                float pr = cr * v + 255.0f * head;
                float pg = cg * v + 255.0f * head;
                float pb = cb * v + 255.0f * head;

                strip.setPixel(i, (uint8_t)(pr > 255 ? 255 : pr),
                               (uint8_t)(pg > 255 ? 255 : pg),
                               (uint8_t)(pb > 255 ? 255 : pb));
            }
        }
        else
        {
            // filled: a slow pulse that starts at full (cos begins at 1 with
            // zero slope, so it continues smoothly from the just-filled strip)
            // and breathes down to the floor and back — that first descent is
            // the "fade out" before the breathing settles in. A gentle wave
            // travels outward from the button end so the glow feels sourced
            // there; its depth eases in over the first couple of seconds so the
            // spatial ripple grows in rather than snapping on at the handoff.
            float s = t - spread;
            float breath = minLevel + (1.0f - minLevel) *
                           (0.5f + 0.5f * cosf(2.0f * (float)M_PI * s / period));
            float amp = 0.2f * (s < 2.0f ? motion::ease(s / 2.0f) : 1.0f);

            for (int i = 0; i < leds; i++)
            {
                float p = fabsf(i - originLed);
                float x = maxDist > 0 ? p / maxDist : 0.0f;
                float ripple = 0.5f + 0.5f * sinf((x * 1.2f - s / period)
                                                  * 2.0f * (float)M_PI);
                float v = breath * (1.0f - amp + amp * ripple);
                if (v > 1) v = 1;

                uint8_t cr, cg, cb;
                palette.at(x, cr, cg, cb);

                strip.setPixel(i, (uint8_t)(cr * v), (uint8_t)(cg * v),
                               (uint8_t)(cb * v));
            }
        }
    }

    // holds the pulse until the host takes over (like boot)
    bool finished() const override { return false; }

private:
    color::Gradient palette;
    float spread = 2.5f;
    float period = 4.0f;
    float minLevel = 0.25f;
    float origin = 1.0f;
};

REGISTER_EFFECT("button_on", ButtonOn)
