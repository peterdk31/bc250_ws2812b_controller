#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "audio.hpp"

// beats as water: each bass hit drops a soft ring at the center of the
// strip that races outward toward both tips, decelerating and fading
// like a real ripple. Ring colors walk the palette one step per beat,
// so a run of hits paints a sequence rather than repeating one color.
// Underneath, a dim aurora-style wash breathes with the overall level,
// so sustained music glows gently between hits. Audio levels come from
// audio::Levels (capture and auto-gain shared by the audio family).
//
// Beat detection is a rising edge of the bass envelope over its own
// slow average — no thresholds tuned to a particular volume, and
// nothing fires in silence.
//
// config:
//   palette            ring colors, walked one step per beat
//                      (default deep blue -> cyan -> violet)
//   glow_color         RRGGBB newborn rings lean toward (default ffffff)
//   speed              floor wash drift rate multiplier (default 1.0)
//   noise              flow/noise blend for the wash (default 0.3)
//   floor_brightness   dim wash on the track 0..1 (default 0.05)
//   idle_brightness    ambient wash during in-condition silence (default 0.25)
//   idle_seconds       quiet time before the idle swell (default 1.5)
//   width              ring half-width as a fraction of the half-strip
//                      (default 0.07)
//   travel_seconds     a ring's center -> tip journey time (default 1.0)
//   sensitivity        beat threshold scale; higher fires on softer hits
//                      (default 1.0)
//   attack_seconds / release_seconds / gain_seconds   as in `audio_music`
class Ripple : public Effect
{
public:
    ~Ripple() override
    {
        if (acquired)
            audio::Levels::shared().release();
    }

    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "0030a0,00c0e0,8040ff"));
        speed = cfg.getFloat("speed", 1.0f);
        noiseMix = cfg.getFloat("noise", 0.3f);
        floorLevel = cfg.getFloat("floor_brightness", 0.05f);
        idleBright = cfg.getFloat("idle_brightness", 0.25f);
        idleSeconds = cfg.getFloat("idle_seconds", 1.5f);

        width = cfg.getFloat("width", 0.07f);
        if (width < 0.02f) width = 0.02f;
        travel = cfg.getFloat("travel_seconds", 1.0f);
        if (travel < 0.1f) travel = 0.1f;
        sens = cfg.getFloat("sensitivity", 1.0f);
        if (sens < 0.1f) sens = 0.1f;

        uint32_t gc = cfg.getColor("glow_color", 0xffffff);
        glowR = (gc >> 16) & 0xFF;
        glowG = (gc >> 8) & 0xFF;
        glowB = gc & 0xFF;

        for (Ring& r : rings)
            r.age = 1e9f;

        audio::Levels& lv = audio::Levels::shared();

        lv.configure(cfg.getFloat("attack_seconds", 0.035f),
                     cfg.getFloat("release_seconds", 0.3f),
                     cfg.getFloat("gain_seconds", 6.0f));
        lv.acquire();
        acquired = true;
    }

    void render(Strip& strip, float t) override
    {
        float dt = frameDelayMs() / 1000.0f;

        audio::Levels& lv = audio::Levels::shared();
        lv.update(dt);

        float lev = lv.level();
        float bass = lv.bass();

        // a beat is the bass envelope popping above its own slow average
        // (self-scaling, like the auto-gain). One ring per pop: fire on
        // the rising edge, re-arm once the envelope has clearly fallen
        avg += (1.0f - expf(-dt / 1.5f)) * (bass - avg);

        bool hot = bass > avg + 0.22f / sens && bass > 0.25f / sens;

        if (hot && armed && t - lastSpawn > 0.12f)
        {
            spawn(bass);
            lastSpawn = t;
            armed = false;
        }
        else if (!hot && bass < avg + 0.08f / sens)
        {
            armed = true;
        }

        for (Ring& r : rings)
            r.age += dt;

        // silence grace, as in `audio_music`: quiet-but-active swells the wash
        float idleTarget = lv.quietSeconds() > idleSeconds ? 1.0f : 0.0f;
        idle += (1.0f - expf(-dt / 0.8f)) * (idleTarget - idle);

        wash += dt;

        float inv2w2 = 1.0f / (2.0f * width * width);

        int leds = strip.size();
        float half = leds / 2.0f;

        for (int i = 0; i < leds; i++)
        {
            // distance from the strip center, 0..1 — rings live in the
            // same mirrored coordinate as the rest of the family
            float d = i < half ? (half - i - 0.5f) / half
                               : (i + 0.5f - half) / half;

            // dim breathing floor plus a soft center glow that follows
            // the overall level, so the water is never dead between hits
            float glow = 0.30f * lev * expf(-d * d * (1.0f / 0.065f));
            float fl = floorLevel + (idleBright - floorLevel) * idle;
            float base = fl + glow;

            float w = motion::mix(motion::flow(d, wash, speed),
                                  motion::noise(d, wash, speed), noiseMix);
            float v = 0.6f + 0.4f * motion::shimmer(d, wash, speed);

            uint8_t r, g, b;
            palette.at(w, r, g, b);

            float rr = r * v * base;
            float gg = g * v * base;
            float bb = b * v * base;

            // rings, additive: position decelerates toward the tip (fast
            // birth, gentle arrival), brightness fades with age, and a
            // newborn ring leans toward glow_color before settling into
            // its palette color
            for (const Ring& ring : rings)
            {
                if (ring.age >= travel)
                    continue;

                float u = ring.age / travel;
                float pos = 1.0f - (1.0f - u) * (1.0f - u);

                float dd = d - pos;
                float g2 = expf(-dd * dd * inv2w2)
                    * ring.strength * (1.0f - u);

                if (g2 <= 0.003f)
                    continue;

                uint8_t cr, cg, cb;
                palette.at(ring.color, cr, cg, cb);

                float lean = 0.5f * (1.0f - u);
                float pr = cr + (glowR - cr) * lean;
                float pg = cg + (glowG - cg) * lean;
                float pb = cb + (glowB - cb) * lean;

                rr += pr * g2;
                gg += pg * g2;
                bb += pb * g2;
            }

            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;

            strip.setPixel(i, (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
        }
    }

private:
    struct Ring
    {
        float age = 1e9f;
        float strength = 0;
        float color = 0;
    };

    static const int MAX_RINGS = 6;

    // take the most-faded slot, so a dense run of beats recycles the
    // ring that matters least
    void spawn(float bass)
    {
        Ring* slot = &rings[0];

        for (Ring& r : rings)
            if (r.age > slot->age)
                slot = &r;

        // deterministic variety: each beat takes the next palette step
        colorPhase += 0.17f;
        colorPhase -= floorf(colorPhase);

        slot->age = 0;
        slot->strength = fminf(0.35f + 0.75f * bass, 1.1f);
        slot->color = colorPhase;
    }

    color::Gradient palette;
    float speed = 1.0f;
    float noiseMix = 0.3f;
    float floorLevel = 0.05f;
    float idleBright = 0.25f;
    float idleSeconds = 1.5f;
    float width = 0.07f;
    float travel = 1.0f;
    float sens = 1.0f;
    float glowR = 255, glowG = 255, glowB = 255;

    bool acquired = false;
    Ring rings[MAX_RINGS];
    float avg = 0;
    bool armed = true;
    float lastSpawn = -1e9f;
    float colorPhase = 0;
    float idle = 0;
    float wash = 0;
};

REGISTER_EFFECT("audio_ripple", Ripple)
