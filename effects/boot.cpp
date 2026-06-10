#include "effect.hpp"

// blue loading bar that fills the strip over `duration_seconds`,
// then reports finished so the playlist advances
class Boot : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        duration = cfg.getFloat("duration_seconds", 5.0f);
    }

    void render(WS2812Serial& strip, float t) override
    {
        elapsed = t;

        int leds = strip.size();

        float p = duration > 0 ? t / duration : 1.0f;
        if (p > 1) p = 1;

        int head = (int)(p * leds);

        for (int i = 0; i < leds; i++)
        {
            if (i < head)
                strip.setPixel(i, 0, 40, 255);
            else if (i == head)
                strip.setPixel(i, 180, 200, 255);
            else
                strip.setPixel(i, 0, 0, 0);
        }
    }

    bool finished() const override
    {
        return elapsed >= duration;
    }

private:
    float duration = 5.0f;
    float elapsed = 0.0f;
};

REGISTER_EFFECT("boot", Boot)
