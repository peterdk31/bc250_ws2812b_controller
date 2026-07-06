#include <math.h>
#include <stdio.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "audio.hpp"

// what's playing, as light: a bloom growing from the center of the strip
// outward on both sides, its reach following the loudness of whatever the
// system is playing (audio::Levels — capture and analysis shared by the
// whole audio family). The lit region carries the usual aurora-style
// palette wash, the bass line swells the center and sends soft crests
// travelling out toward the tips, and treble content quickens the wash —
// so the strip pumps, breathes and flows with the music rather than
// flickering at it.
//
// Levels are self-normalising (see audio.hpp), so the effect fills the
// strip at any volume instead of needing gain tuned per source. No
// capture client / no session → it degrades to the dim floor wash.
//
// config:
//   palette            comma-separated stops the wash walks
//                      (default violet -> magenta -> amber)
//   glow_color         RRGGBB the bass crests lean toward (default ffffff)
//   speed              drift / shimmer rate multiplier (default 1.2)
//   noise              flow/noise blend 0 (sine flow) .. 1 (noise) (default 0.3)
//   floor_brightness   dim wash on the unlit track 0..1 (default 0.05)
//   idle_brightness    calm ambient wash the floor swells into when the
//                      audio goes quiet, so the strip is never left blank
//                      while the audio device lingers open after playback
//                      (~5 s under PipeWire) 0..1 (default 0.25)
//   idle_seconds       quiet time before the idle swell starts (default 1.5)
//   attack_seconds     level rise time constant (default 0.035)
//   release_seconds    level fall time constant (default 0.3)
//   gain_seconds       auto-gain window: how fast "loud" adapts (default 6)
//   pulse              bass swell/crest strength, 0 disables (default 0.8)
//   pulse_width        crest half-width as a fraction of the half-strip
//                      (default 0.18)
//   pulse_rate_idle    crest journeys per second in quiet passages (default 0.4)
//   pulse_rate_full    crest journeys per second at full level (default 2.2)
class Music : public Effect
{
public:
    ~Music() override
    {
        if (acquired)
            audio::Levels::shared().release();
    }

    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "4a00b4,e02090,ff9c28"));
        speed = cfg.getFloat("speed", 1.2f);
        noiseMix = cfg.getFloat("noise", 0.3f);
        floorLevel = cfg.getFloat("floor_brightness", 0.05f);
        idleBright = cfg.getFloat("idle_brightness", 0.25f);
        idleSeconds = cfg.getFloat("idle_seconds", 1.5f);

        pulseStrength = cfg.getFloat("pulse", 0.8f);
        pulseWidth = cfg.getFloat("pulse_width", 0.18f);
        if (pulseWidth < 0.02f) pulseWidth = 0.02f;
        rateIdle = cfg.getFloat("pulse_rate_idle", 0.4f);
        rateFull = cfg.getFloat("pulse_rate_full", 2.2f);

        uint32_t gc = cfg.getColor("glow_color", 0xffffff);
        glowR = (gc >> 16) & 0xFF;
        glowG = (gc >> 8) & 0xFF;
        glowB = gc & 0xFF;

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

        float lev = lv.level();
        float bassE = lv.bass();

        // silence grace: once the music has been quiet for a while (but
        // the rule is still holding us active), ease the floor up into a
        // calm ambient wash rather than sitting near-black
        float idleTarget = lv.quietSeconds() > idleSeconds ? 1.0f : 0.0f;
        idle += (1.0f - expf(-dt / 0.8f)) * (idleTarget - idle);

        // integrated clocks (never scale t directly — a rate change would
        // teleport the pattern): the wash quickens with treble sparkle,
        // the crest with overall level
        wash += dt * (1.0f + 0.8f * lv.treble());
        phase += dt * (rateIdle + (rateFull - rateIdle) * lev);

        float crest = phase - floorf(phase);

        // half-sine envelope: the crest is born softly at the center and
        // dies just as it reaches the bloom's tip; its depth is the bass
        float beat = sinf(3.14159265f * crest) * pulseStrength * bassE;

        float inv2w2 = 1.0f / (2.0f * pulseWidth * pulseWidth);

        int leds = strip.size();
        float half = leds / 2.0f;

        for (int i = 0; i < leds; i++)
        {
            // this pixel's center as distance from the strip center, 0..1
            float d = i < half ? (half - i - 0.5f) / half
                               : (i + 0.5f - half) / half;

            // soft tip: full inside the bloom, fading across the single
            // pixel straddling its edge
            float live = (lev - d) * half + 0.5f;
            if (live < 0.0f) live = 0.0f;
            if (live > 1.0f) live = 1.0f;

            float fill = live;

            // the unlit track keeps a faint wash rather than going black,
            // so the bloom rises out of a living floor instead of an edge;
            // in the silence grace the floor swells toward the idle level
            float fl = floorLevel + (idleBright - floorLevel) * idle;
            if (fill < fl) fill = fl;

            float w = motion::mix(motion::flow(d, wash, speed),
                                  motion::noise(d, wash, speed), noiseMix);
            float v = 0.6f + 0.4f * motion::shimmer(d, wash, speed);

            uint8_t r, g, b;
            palette.at(w, r, g, b);

            // the bass, twice: a center-weighted swell so the whole bloom
            // pumps with the low end, and the travelling crest radiating
            // that energy outward; `live` confines both to the bloom
            float dd = d - crest * lev;
            float p = beat * expf(-dd * dd * inv2w2) * live;
            float swell = 0.5f * pulseStrength * bassE * (1.0f - d) * live;

            // the lift brightens and leans the hue toward glow_color, so
            // hits read as energy moving through the wash
            float lift = p + swell;
            if (lift > 1.0f) lift = 1.0f;

            float rr = r + (glowR - r) * lift * 0.6f;
            float gg = g + (glowG - g) * lift * 0.6f;
            float bb = b + (glowB - b) * lift * 0.6f;

            float k = v * (1.0f + p + swell);
            if (k > 1.0f) k = 1.0f;
            k *= fill;

            strip.setPixel(i, (uint8_t)(rr * k), (uint8_t)(gg * k),
                           (uint8_t)(bb * k));
        }
    }

private:
    color::Gradient palette;
    float speed = 1.2f;
    float noiseMix = 0.3f;
    float floorLevel = 0.05f;
    float idleBright = 0.25f;
    float idleSeconds = 1.5f;
    float pulseStrength = 0.8f;
    float pulseWidth = 0.18f;
    float rateIdle = 0.4f;
    float rateFull = 2.2f;
    float glowR = 255, glowG = 255, glowB = 255;

    bool acquired = false;
    float idle = 0;
    float wash = 0, phase = 0;
};

REGISTER_EFFECT("audio_music", Music)
