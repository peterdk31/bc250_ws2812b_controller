#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// classic 1D plasma: several sines at unrelated frequencies and drift
// rates sum per LED into a value that walks a configurable palette,
// blended with value noise so it shifts organically and never repeats.
// Make the palette's first and last stop match for a seamless wrap at the
// extremes.
//
// config:
//   palette  comma-separated stops (default cool blue -> teal -> violet,
//            looped: "0040ff,00d0a0,c040ff,0040ff")
//   speed    drift rate multiplier (default 1.0)
//   noise    sine/noise blend 0 (pure sines) .. 1 (pure noise) (default 0.5)
class Plasma : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 33);

        palette = color::Gradient(cfg.get("palette", "0040ff,00d0a0,c040ff,0040ff"));
        speed = cfg.getFloat("speed", 1.0f);
        noiseMix = cfg.getFloat("noise", 0.5f);
    }

    void render(Strip& strip, float t) override
    {
        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // three drifting sines; their sum (range -3..3) folded to 0..1
            float s = sinf(x * 6.3f + t * 0.20f * speed)
                    + sinf(x * 3.1f - t * 0.27f * speed + 1.3f)
                    + sinf(x * 1.7f + t * 0.13f * speed + 4.2f);
            float v = motion::mix(0.5f + s / 6.0f,
                                  motion::noise(x, t, speed, 4.0f), noiseMix);

            uint8_t r, g, b;
            palette.at(v, r, g, b);

            strip.setPixel(i, r, g, b);
        }
    }

private:
    color::Gradient palette;
    float speed = 1.0f;
    float noiseMix = 0.5f;
};

REGISTER_EFFECT("plasma", Plasma)
