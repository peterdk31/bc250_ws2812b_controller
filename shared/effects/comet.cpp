#include <math.h>
#include "effect.hpp"

// "Larson scanner": a bright eye bounces end to end leaving a fading tail
//
// config:
//   color             RRGGBB (default ff0000)
//   sweeps_per_second end-to-end sweeps per second (default 0.5)
//   tail_pixels       tail length (default 8)
class Comet : public Effect
{
public:
    void init(const EffectConfig& cfg, int leds) override
    {
        setFrameDelay(cfg, 16);

        uint32_t color = cfg.getColor("color", 0xff0000);

        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;

        speed = cfg.getFloat("sweeps_per_second", 0.5f);
        float tail = cfg.getFloat("tail_pixels", 8.0f);
        if (tail < 1) tail = 1;

        trail.assign(leds, 0.0f);

        // per-frame decay so the trail drops to ~5% `tail` pixels
        // behind the head
        float headPxPerSec = (leds - 1) * speed;
        float dt = frameDelayMs() / 1000.0f;
        fadePerFrame = headPxPerSec > 0
            ? powf(0.05f, dt * headPxPerSec / tail) : 1.0f;
    }

    void render(Strip& strip, float t) override
    {
        int leds = strip.size();

        for (int i = 0; i < leds; i++)
            trail[i] *= fadePerFrame;

        // triangle wave bounces the head between the strip ends
        float phase = fmodf(t * speed, 2.0f);
        float pos = (phase < 1.0f ? phase : 2.0f - phase) * (leds - 1);

        int head = (int)pos;
        float frac = pos - head;

        bump(head, 1.0f - frac);
        bump(head + 1, frac);

        for (int i = 0; i < leds; i++)
            strip.setPixel(i, (uint8_t)(r * trail[i]),
                           (uint8_t)(g * trail[i]),
                           (uint8_t)(b * trail[i]));
    }

private:
    void bump(int i, float v)
    {
        if (i >= 0 && i < (int)trail.size() && trail[i] < v)
            trail[i] = v;
    }

    uint8_t r = 255, g = 0, b = 0;
    float speed = 0.5f;
    float fadePerFrame = 1.0f;
    std::vector<float> trail;
};

REGISTER_EFFECT("comet", Comet)
