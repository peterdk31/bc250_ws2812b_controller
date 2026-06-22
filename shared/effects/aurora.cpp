#include <math.h>
#include "effect.hpp"

// slow drifting curtains of color: two sine fields blended per LED,
// mapped into a hue band, with a third field varying the brightness
//
// config:
//   speed    drift rate multiplier (default 1.0)
//   hue_min  low end of the hue band 0..1 (default 0.30, green)
//   hue_max  high end of the hue band 0..1 (default 0.85, purple)
class Aurora : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 33);

        speed = cfg.getFloat("speed", 1.0f);
        hueMin = cfg.getFloat("hue_min", 0.30f);
        hueMax = cfg.getFloat("hue_max", 0.85f);
    }

    void render(Strip& strip, float t) override
    {
        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // two waves at unrelated frequencies drifting in opposite
            // directions, so the pattern never visibly repeats
            float w = 0.5f + 0.25f * sinf(x * 5.1f + t * 0.31f * speed)
                           + 0.25f * sinf(x * 2.3f - t * 0.17f * speed);

            float h = hueMin + (hueMax - hueMin) * w;
            float v = 0.55f + 0.45f * sinf(x * 3.7f + t * 0.23f * speed + 1.7f);

            uint8_t r, g, b;
            hsv_to_rgb(h, v, r, g, b);

            strip.setPixel(i, r, g, b);
        }
    }

private:
    float speed = 1.0f;
    float hueMin = 0.30f;
    float hueMax = 0.85f;

    static void hsv_to_rgb(float h, float v, uint8_t &r, uint8_t &g, uint8_t &b)
    {
        h = fmodf(h, 1.0f);
        if (h < 0) h += 1.0f;

        float c = v;
        float x = c * (1 - fabsf(fmodf(h * 6, 2) - 1));

        float rp, gp, bp;

        int i = (int)(h * 6);

        switch (i)
        {
            case 0: rp = c; gp = x; bp = 0; break;
            case 1: rp = x; gp = c; bp = 0; break;
            case 2: rp = 0; gp = c; bp = x; break;
            case 3: rp = 0; gp = x; bp = c; break;
            case 4: rp = x; gp = 0; bp = c; break;
            default: rp = c; gp = 0; bp = x; break;
        }

        r = (uint8_t)(rp * 255);
        g = (uint8_t)(gp * 255);
        b = (uint8_t)(bp * 255);
    }
};

REGISTER_EFFECT("aurora", Aurora)
