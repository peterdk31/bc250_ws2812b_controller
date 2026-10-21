#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "steam_achievements.hpp"

// A Steam achievement unlocking (steam_achievements.hpp), activated by the
// steam_achievement condition. One shot: a white-hot bloom at the origin
// throws a bright front out along the strip, decelerating as it goes, and
// leaves a palette wash behind it that glints with slow light travelling
// outward; then the whole strip eases down to dark and the effect ends. A
// rare unlock — one few players have — burns in rare_palette instead. A
// second unlock while this one plays starts it over.
//
// config:
//   origin            where the light comes from, 0..1 along the strip
//                     (default 0.5, the center)
//   palette           the wash by distance from the origin, origin -> ends
//                     (default ff8a00,ffc040,fff0b0: gold)
//   rare_palette      the same for a rare unlock (default
//                     ff3cac,8a4bff,38c8ff: prismatic)
//   rare_below        an unlock is rare when fewer than this % of players
//                     have it (default 10; 0 never)
//   flash_color       the bloom and the front RRGGBB (default fffbe8)
//   duration_seconds  the whole show, fade included (default 4.5)
//   spread_seconds    time for the front to reach the ends (default 0.9)
//   fade_seconds      the closing ease to dark (default 1.5)
//   glint             depth of the travelling glints on the wash, 0..1
//                     (default 0.35)
class Achievement : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        origin = cfg.getFloat("origin", 0.5f);
        if (origin < 0) origin = 0;
        if (origin > 1) origin = 1;

        common = color::Gradient(cfg.get("palette", "ff8a00,ffc040,fff0b0"));
        rare = color::Gradient(cfg.get("rare_palette", "ff3cac,8a4bff,38c8ff"));
        rareBelow = cfg.getFloat("rare_below", 10.0f);

        uint32_t flash = cfg.getColor("flash_color", 0xfffbe8);
        fr = (flash >> 16) & 0xFF;
        fg = (flash >> 8) & 0xFF;
        fb = flash & 0xFF;

        duration = cfg.getFloat("duration_seconds", 4.5f);
        spread = cfg.getFloat("spread_seconds", 0.9f);
        if (spread <= 0) spread = 0.9f;
        fade = cfg.getFloat("fade_seconds", 1.5f);
        if (fade > duration) fade = duration;
        if (fade < 0.01f) fade = 0.01f;
        glint = cfg.getFloat("glint", 0.35f);

        unlockAt = -1e9;
        start = 0;
        done = false;
    }

    void render(Strip& strip, float t) override
    {
        const steam::Unlock& u = steam::lastUnlock();
        if (u.at != unlockAt)
        {
            unlockAt = u.at;
            start = t;
            isRare = u.global >= 0 && u.global < rareBelow;
            done = false;
        }

        float s = t - start;
        done = s >= duration;

        int leds = strip.size();
        float originLed = origin * (leds - 1);
        float maxDist = fmaxf(originLed, (leds - 1) - originLed);
        float feather = (maxDist + 1.0f) * 0.15f + 1.0f;

        // ease-out front: fastest out of the bloom, settling at the ends
        float sp = s / spread;
        float travel = 1.0f - powf(1.0f - fminf(sp, 1.0f), 2.5f);
        float front = travel * (maxDist + 0.5f * feather);
        float frontGlow = sp < 1.0f ? 0.4f + 0.6f * (1.0f - sp) : 0.0f;

        float bloom = expf(-s / 0.35f);
        float bloomWidth = 1.0f + 3.0f * s;

        float env = 1.0f - motion::ease((s - (duration - fade)) / fade);
        if (done) env = 0;

        const color::Gradient& palette = isRare ? rare : common;

        for (int i = 0; i < leds; i++)
        {
            float p = fabsf(i - originLed);
            float x = maxDist > 0 ? p / maxDist : 0.0f;

            float cov = motion::ease((front - p) / feather + 0.5f);

            // glints: a fine outward-running sine under a broad slower one,
            // so highlights travel outward and swell rather than blink
            float g = (0.5f + 0.5f * sinf(p * 1.3f - s * 6.0f)) *
                      (0.5f + 0.5f * sinf(p * 0.37f - s * 2.1f + 1.1f));
            float v = cov * (1.0f - glint + glint * 2.0f * g);

            uint8_t cr, cg, cb;
            palette.at(x, cr, cg, cb);

            float dh = p - front;
            float hot = frontGlow * expf(-(dh * dh) / (2.0f * 1.2f * 1.2f)) +
                        bloom * expf(-(p * p) / (2.0f * bloomWidth * bloomWidth));

            float R = (cr * v + fr * hot) * env;
            float G = (cg * v + fg * hot) * env;
            float B = (cb * v + fb * hot) * env;

            strip.setPixel(i, clamp(R), clamp(G), clamp(B));
        }
    }

    bool finished() const override { return done; }

private:
    static uint8_t clamp(float v)
    {
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint8_t)v;
    }

    float origin = 0.5f;
    color::Gradient common, rare;
    float rareBelow = 10.0f;
    uint8_t fr = 0xff, fg = 0xfb, fb = 0xe8;
    float duration = 4.5f;
    float spread = 0.9f;
    float fade = 1.5f;
    float glint = 0.35f;

    double unlockAt = -1e9;     // the unlock playing
    float start = 0;            // effect time it started at
    bool isRare = false;
    bool done = false;
};

REGISTER_EFFECT("achievement", Achievement)
