#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "audio.hpp"

// beats as water: each bass hit drops a wave crest at the center of the
// strip that races outward toward both tips, decelerating, widening as
// it disperses, and dragging a luminous wake over the water it has
// crossed. At the tip it reflects, lapping faintly back toward the
// center like a wave off a pool wall. Ring colors walk the palette one
// step per beat, so a run of hits paints a sequence rather than
// repeating one color. Underneath, a soft center glow breathes with the
// overall level and widens its reach with the bass, and treble sparkle
// quickens the surface wash — so the water lives between hits instead
// of waiting for them. Audio levels come from audio::Levels (capture
// and auto-gain shared by the audio family).
//
// Beat detection runs on audio::Levels' fast-release bass envelope,
// popping clear of its own recent trough by a *ratio*, with ratio
// hysteresis before the next ring — self-scaling at any volume,
// consistent from quiet grooves to loud drops, and nothing fires in
// silence or on sustained (beatless) bass.
//
// config:
//   palette            ring colors, walked one step per beat
//                      (default deep blue -> cyan -> violet)
//   glow_color         RRGGBB newborn rings lean toward (default ffffff)
//   speed              glow wash drift rate multiplier (default 1.0)
//   noise              flow/noise blend for the wash (default 0.3)
//   width              crest half-width as a fraction of the half-strip
//                      at birth; crests widen as they disperse
//                      (default 0.07)
//   wake               luminous tail behind each crest, in crest widths
//                      (default 2.0, 0 for clean rings)
//   reflect            how much of a crest survives the tip and laps
//                      back toward the center, 0..1 (default 0.6,
//                      0 disables the reflection)
//   travel_seconds     a ring's center -> tip journey time; the lap
//                      back takes the same again (default 1.0)
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

        width = cfg.getFloat("width", 0.07f);
        if (width < 0.02f) width = 0.02f;
        wake = cfg.getFloat("wake", 2.0f);
        if (wake < 0.0f) wake = 0.0f;
        reflectAmt = cfg.getFloat("reflect", 0.6f);
        if (reflectAmt < 0.0f) reflectAmt = 0.0f;
        if (reflectAmt > 1.0f) reflectAmt = 1.0f;
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
        float bass = lv.bassFast();

        // a beat is the fast bass envelope popping clear of its own
        // recent trough (bassFast() — the display envelope's slow
        // release leaves no valley between beats to pop out of). The
        // trough follower snaps down with every dip and drifts up
        // slowly; both thresholds are ratios of it, so quiet grooves
        // and loud drops fire alike, and re-arming needs a fall back
        // toward the trough — a sustained beatless swell never dips,
        // so it fires exactly once, at its onset.
        if (bass < low)
            low = bass;
        else
            low += (1.0f - expf(-dt / 1.5f)) * (bass - low);

        if (armed && bass > low * (1.0f + 0.35f / sens) + 0.02f / sens &&
            t - lastSpawn > 0.15f)
        {
            // ring strength from the pop ratio, not the raw level, so
            // a quiet groove's hits ripple as boldly as a loud drop's
            spawn(fminf((bass / (low + 0.02f) - 1.0f) / 1.2f, 1.0f));
            lastSpawn = t;
            armed = false;
        }
        else if (!armed && bass < low * (1.0f + 0.25f / sens) + 0.01f)
        {
            armed = true;
        }

        for (Ring& r : rings)
            r.age += dt;

        // the water between hits follows the level through a deliberately
        // lazy envelope: the raw one is tuned to catch hits, and at low
        // level the auto-gain makes it twitchy — ambience should breathe
        glowLev += (1.0f - expf(-dt / 0.35f)) * (lev - glowLev);

        // the glow's reach follows the low end through its own lazy
        // follower: a full bass line widens the water, a sparse one
        // keeps it pooled at the center
        glowBass += (1.0f - expf(-dt / 0.25f)) * (lv.bass() - glowBass);

        // treble sparkle quickens the surface (integrated clock — never
        // scale t directly, a rate change would teleport the pattern)
        wash += dt * (1.0f + 0.8f * lv.treble());

        // resolve each ring once per frame: crests decelerate out to the
        // tip, then — reflected — leave the wall gently and fade on the
        // way back; they widen as they disperse, and a newborn's crest
        // leans toward glow_color before settling into its palette color
        struct Live
        {
            float pos, dir, env, inv2w2, wakeLen, color, lean;
        };
        Live live[MAX_RINGS];
        int nLive = 0;

        float life = reflectAmt > 0.0f ? 2.0f : 1.0f;
        float endLevel = 0.5f * reflectAmt;

        for (const Ring& ring : rings)
        {
            float u = ring.age / travel;

            if (u >= life)
                continue;

            Live& L = live[nLive++];

            float cw = width * (1.0f + 0.5f * u);
            L.inv2w2 = 1.0f / (2.0f * cw * cw);
            L.wakeLen = wake * cw;
            L.color = ring.color;
            L.lean = u < 1.0f ? 0.5f * (1.0f - u) : 0.0f;

            if (u < 1.0f)
            {
                L.pos = 1.0f - (1.0f - u) * (1.0f - u);
                L.dir = 1.0f;
                L.env = ring.strength * (1.0f - (1.0f - endLevel) * u);
            }
            else
            {
                float s = u - 1.0f;
                L.pos = 1.0f - s * s;
                L.dir = -1.0f;
                L.env = ring.strength * endLevel * (2.0f - u);
            }
        }

        int leds = strip.size();
        float half = leds / 2.0f;

        float invG = 1.0f / (0.045f + 0.07f * glowBass);

        for (int i = 0; i < leds; i++)
        {
            // distance from the strip center, 0..1 — rings live in the
            // same mirrored coordinate as the rest of the family
            float d = i < half ? (half - i - 0.5f) / half
                               : (i + 0.5f - half) / half;

            // a soft center glow follows the overall level and swells
            // with the bass, so the water breathes between hits; the
            // rest of the track stays truly dark — a dim floor would
            // sit below one 8-bit step after gamma, where the
            // receiver's dither reads as jitter
            float base = 0.30f * glowLev * expf(-d * d * invG);

            float w = motion::mix(motion::flow(d, wash, speed),
                                  motion::noise(d, wash, speed), noiseMix);
            float v = 0.6f + 0.4f * motion::shimmer(d, wash, speed);

            uint8_t r, g, b;
            palette.at(w, r, g, b);

            float rr = r * v * base;
            float gg = g * v * base;
            float bb = b * v * base;

            // crests, additive: a clean gaussian front, and behind it
            // the wake — a luminous exponential tail dragged over the
            // water the crest has already crossed
            for (int j = 0; j < nLive; j++)
            {
                const Live& L = live[j];

                float s = (d - L.pos) * L.dir;
                float g2 = expf(-s * s * L.inv2w2);

                if (s < 0.0f && wake > 0.0f)
                    g2 = fmaxf(g2, 0.45f * expf(s / L.wakeLen));

                g2 *= L.env;

                if (g2 <= 0.003f)
                    continue;

                uint8_t cr, cg, cb;
                palette.at(L.color, cr, cg, cb);

                float pr = cr + (glowR - cr) * L.lean;
                float pg = cg + (glowG - cg) * L.lean;
                float pb = cb + (glowB - cb) * L.lean;

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

    // reflection doubles a ring's life, so a dense run of beats needs a
    // deeper pool before recycling starts stealing visible rings
    static const int MAX_RINGS = 8;

    // take the most-faded slot, so a dense run of beats recycles the
    // ring that matters least; punch is the hit's pop ratio mapped 0..1
    void spawn(float punch)
    {
        Ring* slot = &rings[0];

        for (Ring& r : rings)
            if (r.age > slot->age)
                slot = &r;

        // deterministic variety: each beat takes the next palette step
        colorPhase += 0.17f;
        colorPhase -= floorf(colorPhase);

        slot->age = 0;
        slot->strength = 0.35f + 0.75f * punch;
        slot->color = colorPhase;
    }

    color::Gradient palette;
    float speed = 1.0f;
    float noiseMix = 0.3f;
    float width = 0.07f;
    float wake = 2.0f;
    float reflectAmt = 0.6f;
    float travel = 1.0f;
    float sens = 1.0f;
    float glowR = 255, glowG = 255, glowB = 255;

    bool acquired = false;
    Ring rings[MAX_RINGS];
    float low = 0;
    bool armed = true;
    float lastSpawn = -1e9f;
    float colorPhase = 0;
    float glowLev = 0;
    float glowBass = 0;
    float wash = 0;
};

REGISTER_EFFECT("audio_ripple", Ripple)
