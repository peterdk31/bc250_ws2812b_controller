#include <math.h>
#include "effect.hpp"

// a few soft glows wandering slowly back and forth over a dark base, their
// halos overlapping and parting. The calm, satisfying motion of the comet's
// bounce, but soft-edged and several at once instead of one hard dot — a
// quiet night sky rather than blinking sparks. Fully smooth, no randomness.
//
// config:
//   color       glow color RRGGBB (default 6078ff, cool blue-white)
//   base_color  background RRGGBB (default 000014, near-black blue)
//   blobs       number of wandering glows (default 3)
//   width       glow half-width as a fraction of the strip (default 0.10)
//   speed       wander rate multiplier (default 1.4)
class Drift : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        unpack(cfg.getColor("color", 0x6078ff), glow);
        unpack(cfg.getColor("base_color", 0x000014), base);

        blobs = cfg.getInt("blobs", 3);
        if (blobs < 1) blobs = 1;

        width = cfg.getFloat("width", 0.10f);
        if (width < 0.02f) width = 0.02f;

        speed = cfg.getFloat("speed", 1.4f);
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();
        float inv2sigma2 = 1.0f / (2.0f * width * width);

        for (int i = 0; i < leds; i++)
        {
            float x = leds > 1 ? (float)i / (leds - 1) : 0.0f;

            // sum the soft haloes of every glow; each wanders on its own
            // slow sine so they drift at different rates and never line up
            float light = 0.0f;

            for (int k = 0; k < blobs; k++)
            {
                float rate = (0.11f + 0.05f * k) * speed;
                float phase = 1.7f * k;
                float pos = 0.5f + 0.5f * sinf(t * rate + phase);

                float d = x - pos;
                light += expf(-d * d * inv2sigma2);
            }

            if (light > 1.0f) light = 1.0f;

            strip.setPixel(i, mix(base[0], glow[0], light),
                           mix(base[1], glow[1], light),
                           mix(base[2], glow[2], light));
        }
    }

private:
    float glow[3] = {96, 120, 255};
    float base[3] = {0, 0, 20};
    int blobs = 3;
    float width = 0.10f;
    float speed = 1.4f;

    static void unpack(uint32_t c, float out[3])
    {
        out[0] = (c >> 16) & 0xFF;
        out[1] = (c >> 8) & 0xFF;
        out[2] = c & 0xFF;
    }

    static uint8_t mix(float a, float b, float w)
    {
        return (uint8_t)(a + (b - a) * w);
    }
};

REGISTER_EFFECT("drift", Drift)
