#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// slow drifting curtains of color: a flow field (blended with value noise
// for an organic, never-repeating drift) walks each LED through a
// configurable palette, while a separate shimmer field varies the
// brightness so the curtains breathe.
//
// config:
//   palette  comma-separated stops (default aurora green -> blue -> violet)
//   speed    drift rate multiplier (default 1.0)
//   noise    flow/noise blend 0 (pure sine flow) .. 1 (pure noise)
//            (default 0.3)
class Aurora : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 33);

        palette = color::Gradient(cfg.get("palette", "10ff80,10a0ff,8040ff"));
        speed = cfg.getFloat("speed", 1.0f);
        noiseMix = cfg.getFloat("noise", 0.3f);
    }

    void render(Strip& strip, float t) override
    {
        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            float field = motion::mix(motion::flow(x, t, speed),
                                      motion::noise(x, t, speed), noiseMix);
            float v = 0.1f + 0.9f * motion::shimmer(x, t, speed);

            uint8_t r, g, b;
            palette.at(field, r, g, b);

            strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                           (uint8_t)(b * v));
        }
    }

private:
    color::Gradient palette;
    float speed = 1.0f;
    float noiseMix = 0.3f;
};

REGISTER_EFFECT("aurora", Aurora)
