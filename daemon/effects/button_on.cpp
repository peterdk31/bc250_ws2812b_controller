#include <math.h>
#include "effect.hpp"
#include "motion.hpp"

// The default power-on animation (config esp32.power_on).
//
// The idea: the strip's light looks like it spills out of the physical power
// button, which sits just past one end of the strip (the index-0 end by
// default; see `reverse`). A bright white front bursts fast out of the button
// and then spreads more slowly along the strip — an ease-out front: high speed
// at the button, decelerating as it reaches the far end — led by a soft glowing
// head so it reads as light being pushed in rather than a wipe. Once the strip
// is full it settles into a slow white pulse that travels outward from the
// button end, and never ends: it holds until the host daemon's first frame
// takes over.
//
// config:
//   reverse         which end the button is at: false spreads from the index-0
//                   end, true from the last-index end. A plain direction swap —
//                   the strip may be mounted either orientation, horizontal or
//                   vertical (default true)
//   color           RRGGBB (default ffffff)
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

        reverse = cfg.getBool("reverse", true);

        uint32_t color = cfg.getColor("color", 0xffffff);
        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();
        float feather = leds * 0.18f + 1.0f;

        if (t < spread)
        {
            // ease-out front: fast out of the button, slowing as it spreads.
            // 1-(1-u)^p has its highest speed at u=0 and decelerates to a stop.
            // The travel ends at (last index + half a feather), which is where
            // the far pixel reaches full coverage — so the strip finishes
            // filling exactly at t=spread, with no static hold before the pulse.
            float u = t / spread;
            float fp = 1.0f - powf(1.0f - u, 2.5f);
            float front = fp * (leds - 1 + 0.5f * feather);

            for (int i = 0; i < leds; i++)
            {
                // distance from the button end, so the same math spreads
                // either direction depending on where the button sits
                float p = reverse ? (leds - 1 - i) : i;
                float x = leds > 1 ? p / (leds - 1) : 0.0f;

                // white fill behind the feathered front
                float cov = motion::ease((front - p) / feather + 0.5f);

                // a soft bright head that glows a touch ahead of the front,
                // so the light looks like it's being pushed into the strip
                float dh = p - front;
                float head = 0.55f * expf(-(dh * dh) / (2.0f * 1.6f * 1.6f));

                float v = cov * (0.95f + 0.05f * motion::shimmer(x, t)) + head;
                if (v > 1) v = 1;

                strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                               (uint8_t)(b * v));
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
                float p = reverse ? (leds - 1 - i) : i;
                float x = leds > 1 ? p / (leds - 1) : 0.0f;
                float ripple = 0.5f + 0.5f * sinf((x * 1.2f - s / period)
                                                  * 2.0f * (float)M_PI);
                float v = breath * (1.0f - amp + amp * ripple);
                if (v > 1) v = 1;

                strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                               (uint8_t)(b * v));
            }
        }
    }

    // holds the pulse until the host takes over (like boot)
    bool finished() const override { return false; }

private:
    uint8_t r = 255, g = 255, b = 255;
    float spread = 2.5f;
    float period = 4.0f;
    float minLevel = 0.25f;
    bool reverse = true;
};

REGISTER_EFFECT("button_on", ButtonOn)
