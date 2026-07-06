#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "audio.hpp"

// the music's shape: bass, mids and treble as three soft glows — lows
// at the strip center, mids partway out, highs at the tips, mirrored
// on both halves. Each glow breathes wider and brighter with its
// band's energy, its center drifting slightly on the noise field so
// the bands feel alive rather than pinned. Colors come from the
// palette: the bass takes the first stop, mids the middle, treble the
// last, each leaning further along as its band works harder. Audio
// levels come from audio::Levels (capture and auto-gain shared by the
// audio family), so every band reads 0..1 at any volume.
//
// config:
//   palette            three-ish stops, bass -> mid -> treble
//                      (default ember red -> amber -> ice teal)
//   speed              drift / shimmer rate multiplier (default 1.0)
//   noise              flow/noise blend for the floor wash (default 0.3)
//   floor_brightness   dim wash on the track 0..1 (default 0.05)
//   width              band width multiplier (default 1.0)
//   attack_seconds / release_seconds / gain_seconds   as in `audio_music`
class Spectrum : public Effect
{
public:
    ~Spectrum() override
    {
        if (acquired)
            audio::Levels::shared().release();
    }

    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "ff3020,ffb020,30ffd0"));
        speed = cfg.getFloat("speed", 1.0f);
        noiseMix = cfg.getFloat("noise", 0.3f);
        floorLevel = cfg.getFloat("floor_brightness", 0.05f);

        width = cfg.getFloat("width", 1.0f);
        if (width < 0.2f) width = 0.2f;

        audio::Levels& lv = audio::Levels::shared();

        lv.configure(cfg.getFloat("attack_seconds", 0.035f),
                     cfg.getFloat("release_seconds", 0.3f),
                     cfg.getFloat("gain_seconds", 6.0f));
        lv.acquire();
        acquired = true;
    }

    void render(Strip& strip, float t) override
    {
        (void)t;

        float dt = frameDelayMs() / 1000.0f;

        audio::Levels& lv = audio::Levels::shared();
        lv.update(dt);

        // per-band energy, gently curved so near-silence in a band goes
        // properly dark instead of hovering
        float env[3] = {powf(lv.bass(), 1.3f), powf(lv.mid(), 1.3f),
                        powf(lv.treble(), 1.3f)};

        wash += dt;

        // band homes (distance from center, 0..1) and palette stops
        static const float POS[3] = {0.06f, 0.45f, 0.85f};
        static const float SIGMA[3] = {0.16f, 0.13f, 0.11f};
        static const float STOP[3] = {0.06f, 0.5f, 0.94f};

        // per-band gaussian parameters for this frame: the center drifts
        // on the noise field, the width breathes with the band's energy
        float pos[3], inv2s2[3];
        uint8_t cr[3], cg[3], cb[3];

        for (int k = 0; k < 3; k++)
        {
            pos[k] = POS[k]
                + 0.08f * (motion::noise(0.3f * k, wash, 0.5f * speed) - 0.5f);

            float s = SIGMA[k] * width * (0.55f + 0.9f * env[k]);
            inv2s2[k] = 1.0f / (2.0f * s * s);

            // a hard-working band leans a little further along the
            // palette, so intensity also reads as color temperature
            palette.at(STOP[k] + 0.08f * (env[k] - 0.5f), cr[k], cg[k],
                       cb[k]);
        }

        int leds = strip.size();
        float half = leds / 2.0f;

        for (int i = 0; i < leds; i++)
        {
            // distance from the strip center, 0..1 — both halves mirror
            float d = i < half ? (half - i - 0.5f) / half
                               : (i + 0.5f - half) / half;

            float w = motion::mix(motion::flow(d, wash, speed),
                                  motion::noise(d, wash, speed), noiseMix);
            float v = 0.6f + 0.4f * motion::shimmer(d, wash, speed);

            uint8_t fr, fg, fb;
            palette.at(w, fr, fg, fb);

            float rr = fr * v * floorLevel;
            float gg = fg * v * floorLevel;
            float bb = fb * v * floorLevel;

            // the three band glows, additive over the floor
            for (int k = 0; k < 3; k++)
            {
                float dd = d - pos[k];
                float g2 = env[k] * expf(-dd * dd * inv2s2[k]);

                if (g2 <= 0.003f)
                    continue;

                rr += cr[k] * v * g2;
                gg += cg[k] * v * g2;
                bb += cb[k] * v * g2;
            }

            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;

            strip.setPixel(i, (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
        }
    }

private:
    color::Gradient palette;
    float speed = 1.0f;
    float noiseMix = 0.3f;
    float floorLevel = 0.05f;
    float width = 1.0f;

    bool acquired = false;
    float wash = 0;
};

REGISTER_EFFECT("audio_spectrum", Spectrum)
