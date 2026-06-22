#include <math.h>
#include <stdlib.h>
#include "effect.hpp"

// random pixels flare up and fade out over a dim base color
//
// config:
//   color             spark color RRGGBB (default ffffff)
//   base_color        background RRGGBB (default 000020)
//   sparks_per_second new sparks per second (default 6)
//   fade_seconds      spark fade time constant (default 1.0)
class Twinkle : public Effect
{
public:
    void init(const EffectConfig& cfg, int leds) override
    {
        setFrameDelay(cfg, 33);

        uint32_t spark = cfg.getColor("color", 0xffffff);
        uint32_t base = cfg.getColor("base_color", 0x000020);

        for (int c = 0; c < 3; c++)
        {
            sparkRgb[c] = (spark >> (16 - c * 8)) & 0xFF;
            baseRgb[c] = (base >> (16 - c * 8)) & 0xFF;
        }

        rate = cfg.getFloat("sparks_per_second", 6.0f);
        fade = cfg.getFloat("fade_seconds", 1.0f);
        if (fade <= 0) fade = 0.1f;

        level.assign(leds, 0.0f);
        spawnAcc = 0.0f;
    }

    void render(Strip& strip, float) override
    {
        float dt = frameDelayMs() / 1000.0f;
        float decay = expf(-dt / fade);

        spawnAcc += rate * dt;

        while (spawnAcc >= 1.0f)
        {
            level[rand() % level.size()] = 1.0f;
            spawnAcc -= 1.0f;
        }

        for (int i = 0; i < strip.size(); i++)
        {
            level[i] *= decay;

            uint8_t rgb[3];

            for (int c = 0; c < 3; c++)
                rgb[c] = (uint8_t)(baseRgb[c]
                    + level[i] * (sparkRgb[c] - baseRgb[c]));

            strip.setPixel(i, rgb[0], rgb[1], rgb[2]);
        }
    }

private:
    float sparkRgb[3] = {255, 255, 255};
    float baseRgb[3] = {0, 0, 32};
    float rate = 6.0f;
    float fade = 1.0f;
    float spawnAcc = 0.0f;
    std::vector<float> level;
};

REGISTER_EFFECT("twinkle", Twinkle)
