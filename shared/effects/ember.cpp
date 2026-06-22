#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// slow warm flow: the drifting motion of the aurora, in an ember palette.
// The shared flow field walks each LED through a configurable warm
// gradient while the shimmer field swells the brightness, so the strip
// glows and breathes like coals instead of twitching like a flame. No
// randomness.
//
// config:
//   palette         comma-separated warm stops (default "2a0a00,ff7d1e",
//                   deep red -> amber; add stops like ",ffd060" for gold)
//   speed           drift rate multiplier (default 1.0)
//   min_brightness  floor 0..1 so it never goes fully dark (default 0.18)
//   noise           flow/noise blend 0 (sine flow) .. 1 (noise) (default 0.4)
class Ember : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 33);

        palette = color::Gradient(cfg.get("palette", "2a0a00,ff7d1e"));
        speed = cfg.getFloat("speed", 1.0f);
        minBright = cfg.getFloat("min_brightness", 0.18f);
        noiseMix = cfg.getFloat("noise", 0.4f);
    }

    void render(Strip& strip, float t) override
    {
        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // tighter spatial frequencies than the aurora so the warm
            // bands sit closer together; blended with noise so the coals
            // shift organically, like a bed of embers
            float w = motion::mix(motion::flow(x, t, speed, 4.0f, 1.7f),
                                  motion::noise(x, t, speed, 5.0f), noiseMix);
            float br = minBright + (1.0f - minBright) * motion::shimmer(x, t, speed);

            uint8_t r, g, b;
            palette.at(w, r, g, b);

            strip.setPixel(i, (uint8_t)(r * br), (uint8_t)(g * br),
                           (uint8_t)(b * br));
        }
    }

private:
    color::Gradient palette;
    float speed = 1.0f;
    float minBright = 0.18f;
    float noiseMix = 0.4f;
};

REGISTER_EFFECT("ember", Ember)
