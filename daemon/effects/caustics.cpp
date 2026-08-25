#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// underwater light caustics: two noise fields drifting at different scales
// and in opposite directions are *multiplied* (not summed), which carves
// the light into bright wandering filaments — bright only where both
// fields peak — that brighten, merge and dissolve like sunlight on a pool
// floor. The intensity drives both brightness and the palette position, so
// the deep water sits at the palette's dark first stop and the hottest
// filament crossings reach its pale last stop. Pure function of position
// and time, trivially cheap.
//
// config:
//   palette         stops sampled deep-water -> brightest filament
//                   (default "020818,004060,00b0c0,a0fff0",
//                   abyss -> deep blue -> cyan -> pale foam)
//   speed           drift rate multiplier (default 1.0)
//   scale           noise cells across the strip for the first field
//                   (default 3.0); the second runs ~1.7x finer
//   sharp           filament sharpening exponent (default 1.5); higher
//                   gives thinner, rarer filaments over darker water
//   gain            pre-sharpen gain on the product (default 3.2); higher
//                   lets more of the strip reach the bright stops
//   min_brightness  floor 0..1 so the deep water isn't fully black
//                   (default 0.0)
class Caustics : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette",
                                          "020818,004060,00b0c0,a0fff0"));
        speed = cfg.getFloat("speed", 1.0f);
        scale = cfg.getFloat("scale", 3.0f);
        if (scale < 0.5f) scale = 0.5f;
        sharp = cfg.getFloat("sharp", 1.5f);
        if (sharp < 0.5f) sharp = 0.5f;
        gain = cfg.getFloat("gain", 3.2f);
        if (gain < 0.1f) gain = 0.1f;
        minBright = cfg.getFloat("min_brightness", 0.0f);
    }

    void render(Strip& strip, float t) override
    {
        // skip the noise lattice's opening rows — around t=0 they happen to
        // hash low everywhere, which read as a seconds-long black fade-in
        float tt = t + 37.0f;

        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // two fields, offset in noise space, drifting opposite ways at
            // unrelated rates; their product is high only where both peak
            float n1 = motion::noise(x, tt, speed, scale);
            float n2 = motion::noise(x + 13.7f, tt, -0.9f * speed,
                                     scale * 1.7f);

            float v = n1 * n2 * gain;
            if (v > 1.0f) v = 1.0f;
            v = powf(v, sharp);

            // a slow swell so even the settled water gently surges
            v *= 0.85f + 0.15f * motion::shimmer(x, t, 0.5f * speed);

            float br = minBright + (1.0f - minBright) * v;

            uint8_t r, g, b;
            palette.at(v, r, g, b);

            strip.setPixel(i, (uint8_t)(r * br), (uint8_t)(g * br),
                           (uint8_t)(b * br));
        }
    }

private:
    color::Gradient palette;
    float speed = 1.0f;
    float scale = 3.0f;
    float sharp = 1.8f;
    float gain = 2.6f;
    float minBright = 0.0f;
};

REGISTER_EFFECT("caustics", Caustics)
