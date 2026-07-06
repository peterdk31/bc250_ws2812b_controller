#include "effect.hpp"

// every pixel one fixed color. Not much of an effect — it exists for
// calibrating white_balance and gamma (`./led config.json solid` puts up
// full white to judge the tint; a fractional `level` puts up the dim white
// that exposes gamma drift), and it doubles as a plain static light.
//
// config:
//   color   RRGGBB (default "ffffff", full white for calibration)
//   level   0..1 scale on the color (default 1.0); the scaled value goes
//           through the normal gamma/white-balance LUT like any effect's
//           pixels, so what you tune here is what every effect gets
class Solid : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        // static output: refresh slowly, just enough to survive a
        // receiver reconnect
        setFrameDelay(cfg, 250);

        uint32_t color = cfg.getColor("color", 0xffffff);

        float level = cfg.getFloat("level", 1.0f);
        if (level < 0) level = 0;
        if (level > 1) level = 1;

        r = (uint8_t)(((color >> 16) & 0xFF) * level + 0.5f);
        g = (uint8_t)(((color >> 8) & 0xFF) * level + 0.5f);
        b = (uint8_t)((color & 0xFF) * level + 0.5f);
    }

    void render(Strip& strip, float) override
    {
        for (int i = 0; i < strip.size(); i++)
            strip.setPixel(i, r, g, b);
    }

private:
    uint8_t r = 255, g = 255, b = 255;
};

REGISTER_EFFECT("solid", Solid)
