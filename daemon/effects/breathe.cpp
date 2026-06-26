#include <math.h>
#include "effect.hpp"
#include "color.hpp"

// whole-strip single color on a slow breath, with a gentle band of extra
// brightness that drifts along the strip so the light has soft motion and
// depth instead of every pixel pulsing in lockstep. Calm, like a slow tide.
//
// config:
//   palette         comma-separated stops sampled by breath level, so the
//                   glow shifts hue from trough to peak (default
//                   "0d4a44,30e090", deep teal -> soft green). Use a single
//                   stop for a solid color, e.g. "30d090".
//   period_seconds  seconds per breath (default 3.5)
//   min_brightness  floor 0..1 so the strip never fully blanks (default 0.05)
//   wave_depth      how much the drifting band varies brightness 0..1
//                   (default 0.30); 0 reproduces the old flat breathe
class Breathe : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "0d4a44,30e090"));

        period = cfg.getFloat("period_seconds", 3.5f);
        if (period <= 0) period = 3.5f;

        minLevel = cfg.getFloat("min_brightness", 0.05f);
        waveDepth = cfg.getFloat("wave_depth", 0.30f);
    }

    void render(Strip& strip, float t) override
    {
        // global breath: whole strip rises and falls together
        float breath = 0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI / period);
        float base = minLevel + (1.0f - minLevel) * breath;

        int leds = strip.size();

        for (int i = 0; i < leds; i++)
        {
            float x = leds > 1 ? (float)i / (leds - 1) : 0.0f;

            // a soft brightness band sliding along the strip, one slow
            // drift per couple of breaths, dipping each pixel by up to
            // wave_depth so the glow gently flows end to end
            float ripple = 0.5f + 0.5f * sinf((x * 1.5f - t / period)
                                              * 2.0f * (float)M_PI);
            float v = base * (1.0f - waveDepth + waveDepth * ripple);

            // hue follows brightness: the dim trough takes the first stop,
            // the bright peak the last
            uint8_t cr, cg, cb;
            palette.at(v, cr, cg, cb);

            strip.setPixel(i, (uint8_t)(cr * v), (uint8_t)(cg * v),
                           (uint8_t)(cb * v));
        }
    }

private:
    color::Gradient palette;
    float period = 3.5f;
    float minLevel = 0.05f;
    float waveDepth = 0.30f;
};

REGISTER_EFFECT("breathe", Breathe)
