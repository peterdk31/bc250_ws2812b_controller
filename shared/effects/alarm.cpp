#include <math.h>
#include "effect.hpp"

// whole-strip "something is wrong", as a heartbeat rather than a flat
// blink: each beat is a sharp lub-dub double pulse, and the pulse radiates
// from the center outward so the strip throbs instead of strobing. Stays
// urgent (sharp attack, never fully dark), just deliberate about it.
//
// config:
//   color              RRGGBB (default ff0000)
//   pulses_per_second  heartbeats per second (default 2)
//   min_brightness     floor 0..1 so it never goes dark (default 0.16)
//   radiate            center-to-edge delay as a fraction of a beat
//                      (default 0.12); 0 throbs the whole strip together
class Alarm : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        uint32_t color = cfg.getColor("color", 0xff0000);

        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;

        rate = cfg.getFloat("pulses_per_second", 2.0f);
        minLevel = cfg.getFloat("min_brightness", 0.16f);
        radiate = cfg.getFloat("radiate", 0.12f);
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();
        float center = (leds - 1) / 2.0f;
        float beat = t * rate;

        for (int i = 0; i < leds; i++)
        {
            // distance from the strip center, 0..1
            float dist = center > 0 ? fabsf(i - center) / center : 0.0f;

            // outer pixels lag, so each beat starts at the center and
            // travels out to the ends
            float env = heartbeat(beat - dist * radiate);
            float v = minLevel + (1.0f - minLevel) * env;

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

private:
    uint8_t r = 255, g = 0, b = 0;
    float rate = 2.0f;
    float minLevel = 0.16f;
    float radiate = 0.12f;

    // lub-dub: a strong pulse then a softer one a short beat later, the
    // rest of the cycle at rest. phase is in beats; only the fractional
    // part matters.
    static float heartbeat(float phase)
    {
        float lub = pulseAt(phase, 0.05f, 0.055f);
        float dub = 0.65f * pulseAt(phase, 0.27f, 0.060f);
        return lub > dub ? lub : dub;
    }

    // narrow bump centered on c (width w) over a unit-period phase,
    // wrapping so a pulse near the cycle boundary stays smooth
    static float pulseAt(float phase, float c, float w)
    {
        float d = phase - c;
        d -= floorf(d + 0.5f);            // wrap to (-0.5, 0.5]
        return expf(-(d * d) / (2.0f * w * w));
    }
};

REGISTER_EFFECT("alarm", Alarm)
