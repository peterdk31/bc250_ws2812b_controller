#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// "binary stars": pairs of soft glows orbit a shared center, seen edge-on.
// Each pair slides together, whips through conjunction and eases apart —
// the projected motion of a circular orbit, sped up near closest approach
// (Kepler-style) so a crossing lands as an event rather than a drive-by.
// Where the two bodies cross, their fields add and bloom white-hot, using
// the same additive-field math as lava; with two pairs the inner orbit runs
// faster than the outer, so single, double and quadruple line-ups all
// happen in their own time. Pure function of position and time.
//
// config:
//   palette         stops each body's color window slides through, sampled
//                   faint-halo -> bright-core (default "1030ff,00d0ff,
//                   c080ff,ff40a0", blue -> cyan -> violet -> pink)
//   pairs           orbiting pairs; pair 0 is the widest orbit and each
//                   inner pair is smaller and (Kepler) faster (default 2)
//   origin          center of the orbits along the strip, 0..1
//                   (default 0.5 — the power button on this case)
//   radius          body size as a fraction of the strip (default 0.12)
//   speed           orbital rate multiplier (default 1.0)
//   eccentricity    conjunction speed-up 0 (uniform circle) .. 0.45 (hard
//                   whip through the crossing) (default 0.3)
//   wobble          how far each pair's center wanders around origin on a
//                   slow noise walk (default 0.06); 0 pins the orbits
//   color_speed     how fast each body's palette window slides (default 1.0)
//   color_span      how much of the palette a body's halo->core ramp spans,
//                   0..1 (default 0.5)
//   core            field value mapping to full brightness / the window's
//                   hot end; a lone body peaks at ~1, a conjunction at ~2,
//                   so the default 1.5 makes crossings overshoot and bloom
//                   (default 1.5)
//   white_hot       how strongly a crossing blooms toward white from the
//                   intensity past `core`; 0 disables (default 2.0)
//   min_brightness  floor 0..1 so the space between bodies isn't fully
//                   black (default 0.0)
class Orbit : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette",
                                          "1030ff,00d0ff,c080ff,ff40a0"));
        pairs = cfg.getInt("pairs", 2);
        if (pairs < 1) pairs = 1;
        if (pairs > kMaxPairs) pairs = kMaxPairs;
        origin = cfg.getFloat("origin", 0.5f);
        radius = cfg.getFloat("radius", 0.12f);
        if (radius < 0.01f) radius = 0.01f;
        speed = cfg.getFloat("speed", 1.0f);
        ecc = cfg.getFloat("eccentricity", 0.3f);
        if (ecc < 0.0f) ecc = 0.0f;
        if (ecc > 0.45f) ecc = 0.45f;
        wobble = cfg.getFloat("wobble", 0.06f);
        colorSpeed = cfg.getFloat("color_speed", 1.0f);
        colorSpan = cfg.getFloat("color_span", 0.5f);
        if (colorSpan < 0.05f) colorSpan = 0.05f;
        if (colorSpan > 1.0f) colorSpan = 1.0f;
        core = cfg.getFloat("core", 1.5f);
        if (core < 0.1f) core = 0.1f;
        whiteHot = cfg.getFloat("white_hot", 2.0f);
        if (whiteHot < 0.0f) whiteHot = 0.0f;
        minBright = cfg.getFloat("min_brightness", 0.0f);

        // each inner pair orbits tighter and faster (rate ~ amp^-1.5, the
        // Kepler relation), perturbed by the golden ratio's fractional
        // parts so the pairs' line-ups never fall into a visible loop
        for (int p = 0; p < pairs; p++)
        {
            amp[p] = 0.42f * powf(0.55f, (float)p);
            float g = 0.6180339887f * (p + 1);
            float frac = g - floorf(g);
            rate[p] = 0.03f / powf(amp[p], 1.5f) * (0.9f + 0.2f * frac);
        }
    }

    void render(Strip& strip, float t) override
    {
        // resolve every body's position, falloff and palette window once
        // per frame. The orbit angle advances non-uniformly: fastest where
        // the projected bodies cross (cos = 0), slowest at the turnarounds,
        // so a conjunction whips and the extremes linger.
        int n = pairs * 2;
        for (int p = 0; p < pairs; p++)
        {
            float A = 6.2831853f * rate[p] * speed * t;
            float angle = A - ecc * sinf(2.0f * A);
            float c = origin + wobble
                * (motion::noise(11.3f * p + 5.0f, t, 0.3f * speed) - 0.5f);
            float off = amp[p] * cosf(angle);

            for (int s = 0; s < 2; s++)
            {
                int b = p * 2 + s;
                float breathe = 1.0f + 0.15f * sinf(t * 0.4f * speed
                                                    + 2.1f * b);
                body[b].pos = c + (s ? -off : off);
                body[b].inv = 1.0f / (radius * breathe);
                body[b].base = motion::noise(3.7f * b + 17.0f, t, colorSpeed)
                             * (1.0f - colorSpan);
            }
        }

        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // additive falloff fields, exactly as lava: two approaching
            // bodies pile up toward ~2 and fuse into a hot crossing
            float field = 0.0f;
            for (int b = 0; b < n; b++)
            {
                float d = (x - body[b].pos) * body[b].inv;
                float g = 1.0f / (1.0f + d * d);
                w[b] = g * g;
                field += w[b];
            }

            float raw = field / core;
            float c = raw > 1.0f ? 1.0f : raw;
            float bloom = (raw - 1.0f) * whiteHot;
            if (bloom < 0.0f) bloom = 0.0f;
            if (bloom > 1.0f) bloom = 1.0f;

            float br = minBright + (1.0f - minBright) * motion::ease(c);

            float fr = 0.0f, fg = 0.0f, fb = 0.0f;
            for (int b = 0; b < n; b++)
            {
                uint8_t r, g, bl;
                palette.at(body[b].base + c * colorSpan, r, g, bl);
                fr += w[b] * r;
                fg += w[b] * g;
                fb += w[b] * bl;
            }

            float invF = field > 0.0f ? 1.0f / field : 0.0f;
            float cr = fr * invF, cg = fg * invF, cb = fb * invF;
            cr += (255.0f - cr) * bloom;
            cg += (255.0f - cg) * bloom;
            cb += (255.0f - cb) * bloom;

            strip.setPixel(i, (uint8_t)(cr * br), (uint8_t)(cg * br),
                           (uint8_t)(cb * br));
        }
    }

private:
    static const int kMaxPairs = 6;
    struct Body { float pos, inv, base; };
    Body body[kMaxPairs * 2];
    float w[kMaxPairs * 2];
    float amp[kMaxPairs];
    float rate[kMaxPairs];

    color::Gradient palette;
    int pairs = 2;
    float origin = 0.5f;
    float radius = 0.12f;
    float speed = 1.0f;
    float ecc = 0.3f;
    float wobble = 0.06f;
    float colorSpeed = 1.0f;
    float colorSpan = 0.5f;
    float core = 1.5f;
    float whiteHot = 2.0f;
    float minBright = 0.0f;
};

REGISTER_EFFECT("orbit", Orbit)
