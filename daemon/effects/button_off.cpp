#include <math.h>
#include "effect.hpp"
#include "motion.hpp"

// The default power-off animation (config esp32.shutdown).
//
// Roughly the inverse of button_on: the lit strip "flies" back into the power
// button just past the left / index-0 end. The lit region collapses from the
// far end toward the button, accelerating as it goes (so it zips into the
// button at the very end), led by a bright head, and the colour fades to white
// along the way — ending in a white flash at the button that goes dark. It
// reports finished so the receiver can power the strip down after it (like the
// CRT shutdown).
//
// config:
//   reverse           which end the button is at: false collapses toward the
//                     index-0 end, true toward the last-index end. A plain
//                     direction swap for either mount orientation (default true,
//                     matching button_on)
//   color             starting RRGGBB the light fades to white from (default
//                     ffffff; set a colour to see the fade-to-white)
//   duration_seconds  collapse time (default 1.4)
class ButtonOff : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        duration = cfg.getFloat("duration_seconds", 1.4f);
        if (duration <= 0) duration = 1.4f;

        reverse = cfg.getBool("reverse", true);

        uint32_t color = cfg.getColor("color", 0xffffff);
        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;
    }

    void render(Strip& strip, float t) override
    {
        elapsed = t;

        int leds = strip.size();
        float feather = leds * 0.18f + 1.0f;

        float u = t / duration;
        if (u > 1) u = 1;

        // the lit region's right edge collapses toward the button; (1-u*u)
        // accelerates the collapse so it zips into the button near the end
        float edge = (leds - 1 + feather) * (1.0f - u * u);

        // colour fades to white as the light flies back
        float w = motion::ease(u);
        float cr = r + (255 - r) * w;
        float cg = g + (255 - g) * w;
        float cb = b + (255 - b) * w;

        // ease the whole thing to black over the last fifth so it ends dark
        float endFade = u > 0.8f ? 1.0f - (u - 0.8f) / 0.2f : 1.0f;
        if (endFade < 0) endFade = 0;

        for (int i = 0; i < leds; i++)
        {
            // distance from the button end, so the collapse runs toward
            // whichever end the button is on
            float p = reverse ? (leds - 1 - i) : i;
            float x = leds > 1 ? p / (leds - 1) : 0.0f;

            // lit between the button and the collapsing edge, dark beyond it
            float cov = motion::ease((edge - p) / feather + 0.5f);

            // bright head riding the collapsing edge — the gathered light
            float dh = p - edge;
            float head = 0.7f * expf(-(dh * dh) / (2.0f * 1.6f * 1.6f));

            float v = cov * 0.8f + head;
            if (v > 1) v = 1;
            v *= endFade * (0.9f + 0.1f * motion::shimmer(x, t));

            strip.setPixel(i, (uint8_t)(cr * v), (uint8_t)(cg * v),
                           (uint8_t)(cb * v));
        }
    }

    bool finished() const override { return elapsed >= duration; }

private:
    uint8_t r = 255, g = 255, b = 255;
    float duration = 1.4f;
    float elapsed = 0.0f;
    bool reverse = true;
};

REGISTER_EFFECT("button_off", ButtonOff)
