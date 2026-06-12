#include <math.h>
#include <stdlib.h>
#include "effect.hpp"

// three-act power-on sequence, then reports finished so the playlist
// advances:
//   1. a white spark sweeps the strip, leaving an afterglow in the
//      charge color
//   2. the strip charges up behind an eased fill; the charged section
//      ripples like flowing energy and sparks fly off the head
//   3. the whole strip surges to white and fades to black, handing a
//      dark strip to the rule engine
//
// config:
//   duration_seconds  total run time (default 5)
//   color             charge color RRGGBB (default 0028ff)
//   flash_color       spark/head/flash color RRGGBB (default ffffff)
class Boot : public Effect
{
public:
    void init(const EffectConfig& cfg, int leds) override
    {
        duration = cfg.getFloat("duration_seconds", 5.0f);
        if (duration <= 0) duration = 5.0f;

        uint32_t color = cfg.getColor("color", 0x0028ff);

        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;

        uint32_t flash = cfg.getColor("flash_color", 0xffffff);

        fr = (flash >> 16) & 0xFF;
        fg = (flash >> 8) & 0xFF;
        fb = flash & 0xFF;

        trail.assign(leds, 0.0f);
        lastT = 0.0f;
    }

    void render(WS2812Serial& strip, float t) override
    {
        elapsed = t;

        float dt = t - lastT;
        lastT = t;

        // the sweep's afterglow fades on its own clock so it lingers
        // into the fill phase, which then grows over its remains
        float decay = powf(0.02f, dt / (duration * SWEEP_END));

        for (auto& v : trail)
            v *= decay;

        float u = t / duration;
        if (u > 1) u = 1;

        if (u < SWEEP_END)
            sweep(strip, u / SWEEP_END);
        else if (u < FILL_END)
            fill(strip, (u - SWEEP_END) / (FILL_END - SWEEP_END), t);
        else
            flash(strip, (u - FILL_END) / (1.0f - FILL_END));
    }

    bool finished() const override
    {
        return elapsed >= duration;
    }

private:
    // phase boundaries as fractions of the total duration
    static constexpr float SWEEP_END = 0.18f;
    static constexpr float FILL_END = 0.88f;

    void sweep(WS2812Serial& strip, float p)
    {
        int leds = strip.size();

        // slight acceleration so the spark feels like it's taking off
        float pos = p * (0.5f + 0.5f * p) * (leds - 1);

        int head = (int)pos;
        float frac = pos - head;

        bump(head, 1.0f);
        bump(head + 1, frac);

        paintTrail(strip);

        strip.setPixel(head, fr, fg, fb);
    }

    void fill(WS2812Serial& strip, float p, float t)
    {
        int leds = strip.size();

        // smoothstep: the fill starts gently and brakes into the flash
        float pe = p * p * (3.0f - 2.0f * p);
        float headPos = pe * leds;
        int head = (int)headPos;

        paintTrail(strip);

        for (int i = 0; i < head && i < leds; i++)
        {
            // a wave runs through the charged section so it reads as
            // flowing energy, not a static bar
            float v = 0.7f + 0.3f * sinf(i * 0.6f - t * 9.0f);

            if (trail[i] < v)
                strip.setPixel(i, (uint8_t)(r * v), (uint8_t)(g * v),
                               (uint8_t)(b * v));
        }

        if (head < leds)
            strip.setPixel(head, fr, fg, fb);

        // an occasional spark jumps ahead of the head
        if (rand() % 4 == 0)
        {
            int s = head + 1 + rand() % 4;

            if (s < leds)
                strip.setPixel(s, fr, fg, fb);
        }
    }

    void flash(WS2812Serial& strip, float p)
    {
        const float RISE = 0.25f;

        int leds = strip.size();
        uint8_t rr, gg, bb;

        if (p < RISE)
        {
            // surge from the charge color to the flash color
            float k = p / RISE;

            rr = (uint8_t)(r + (fr - r) * k);
            gg = (uint8_t)(g + (fg - g) * k);
            bb = (uint8_t)(b + (fb - b) * k);
        }
        else
        {
            // quadratic fade to black: fast drop, soft landing
            float k = 1.0f - (p - RISE) / (1.0f - RISE);
            float v = k * k;

            rr = (uint8_t)(fr * v);
            gg = (uint8_t)(fg * v);
            bb = (uint8_t)(fb * v);
        }

        for (int i = 0; i < leds; i++)
            strip.setPixel(i, rr, gg, bb);
    }

    void paintTrail(WS2812Serial& strip)
    {
        for (int i = 0; i < (int)trail.size(); i++)
            strip.setPixel(i, (uint8_t)(r * trail[i]),
                           (uint8_t)(g * trail[i]),
                           (uint8_t)(b * trail[i]));
    }

    void bump(int i, float v)
    {
        if (i >= 0 && i < (int)trail.size() && trail[i] < v)
            trail[i] = v;
    }

    uint8_t r = 0, g = 40, b = 255;
    uint8_t fr = 255, fg = 255, fb = 255;
    float duration = 5.0f;
    float elapsed = 0.0f;
    float lastT = 0.0f;
    std::vector<float> trail;
};

REGISTER_EFFECT("boot", Boot)
