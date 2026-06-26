#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"

// "Lava lamp": a handful of soft blobs (metaballs) drift slowly along the
// strip, bobbing at unrelated rates so they wander, meet and part without
// ever looping. Each blob lays down a smooth field that falls off with
// distance; the fields *add*, so where two blobs overlap the field piles up
// into a brighter, hotter spot — the smooth-union look of signed-distance
// metaballs.
//
// The field is intensity-driven: it sets brightness (dark liquid, glowing
// blobs, bright overlaps) AND the color, by placing each pixel within a
// window of the palette — faint edges at the window's start, hot cores at its
// end. Each blob has its *own* window that slides along the palette over time
// on its own slow, organic noise walk, so the blobs drift through different
// colors and an overlap blends two of them in proportion to how much each
// blob contributes. Each blob still stays intensity-driven within its window.
// Pure function of position and time — no per-frame state, no randomness.
//
// config:
//   palette         comma-separated stops the scene cycles through, sampled
//                   faint-edge -> hot-core within a sliding window
//                   (default "ff0050,ff5a00,ffd000,00e5ff,7a00ff,ff00d0",
//                   rose -> orange -> gold -> cyan -> violet -> magenta).
//   blobs           number of metaballs (default 4)
//   radius          blob size as a fraction of the strip (default 0.2);
//                   at the default the blobs overlap enough to fill the strip
//                   at first and then drift apart and re-merge as they move.
//                   For visibly separate blobs instead, keep it below ~0.5/blobs
//   speed           drift rate of the blobs' motion (default 5.0)
//   bounce          how much the bounce eases, 0..1 (default 0.15). 0 is a
//                   constant-speed glide that reverses cleanly at each end;
//                   1 fully eases, so blobs decelerate to a stop at the walls
//                   and linger there (the old behaviour). Low values keep the
//                   blobs moving uniformly across the strip instead of
//                   crowding the edges.
//   color_speed     how fast each blob's palette window slides (default 1.0)
//   color_span      how much of the palette a blob's edge->core ramp spans,
//                   0..1 (default 0.5). 1.0 uses the whole palette and stops
//                   the windows sliding (every blob the same, pure intensity
//                   coloring); smaller values give tighter, more varied blob
//                   colors that merge into new hues where they overlap.
//   core            field value that maps to the window's hot end / full
//                   brightness; a lone blob peaks at ~1, so the default 1.6
//                   keeps single blobs mid-window and lets overlaps reach the
//                   hot end. Lower it for fatter, hotter blobs (default 1.6)
//   white_hot       how strongly a strong overlap blooms toward white-hot,
//                   from the intensity past `core` (default 2.5). 0 disables
//                   the bloom (overlaps stay palette-colored); higher blows
//                   out sooner and harder. With the default the rare, hardest
//                   pileups flash pure white while ordinary merges only
//                   white-tip; more blobs / larger radius bloom more readily.
//   min_brightness  floor 0..1 so the dark liquid isn't fully black
//                   (default 0.0)
class Lava : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get(
            "palette", "ff0050,ff5a00,ffd000,00e5ff,7a00ff,ff00d0"));
        blobs = cfg.getInt("blobs", 4);
        if (blobs < 1) blobs = 1;
        if (blobs > kMax) blobs = kMax;
        radius = cfg.getFloat("radius", 0.2f);
        if (radius < 0.01f) radius = 0.01f;
        speed = cfg.getFloat("speed", 5.0f);
        bounce = cfg.getFloat("bounce", 0.15f);
        if (bounce < 0.0f) bounce = 0.0f;
        if (bounce > 1.0f) bounce = 1.0f;
        colorSpeed = cfg.getFloat("color_speed", 1.0f);
        colorSpan = cfg.getFloat("color_span", 0.5f);
        if (colorSpan < 0.05f) colorSpan = 0.05f;
        if (colorSpan > 1.0f) colorSpan = 1.0f;
        core = cfg.getFloat("core", 1.6f);
        if (core < 0.1f) core = 0.1f;
        whiteHot = cfg.getFloat("white_hot", 2.5f);
        if (whiteHot < 0.0f) whiteHot = 0.0f;
        minBright = cfg.getFloat("min_brightness", 0.0f);

        // pre-distort each blob's home through pingpong^-1, so that once the
        // (partially) eased bounce is applied in blobPos the blobs land back
        // on their uniform t=0 spacing (the easing would otherwise pull them
        // toward the ends)
        for (int k = 0; k < blobs; k++)
            easedHome[k] = pingpongInverse((k + 0.5f) / blobs, bounce);
    }

    void render(Strip& strip, float t) override
    {
        // resolve each blob's position, falloff scale and the start of its
        // own palette window once per frame (not per pixel). Each window
        // slides on its own slow noise walk (a distinct track per blob), so
        // the blobs drift through different colors.
        int n = blobs;
        for (int k = 0; k < n; k++)
        {
            blob[k].pos = blobPos(k, t);
            blob[k].inv = 1.0f / blobRadius(k, t);
            blob[k].base = motion::noise(3.7f * k, t, colorSpeed)
                         * (1.0f - colorSpan);
        }

        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // each blob's falloff 1/(1+(d/R)^2)^2 peaks at 1 at its center and
            // rolls off with a short tail — short enough that evenly-spaced
            // blobs read as distinct, but the weights still sum, so two
            // centers that approach add toward ~2 and fuse into a hot neck
            float field = 0.0f;
            for (int k = 0; k < n; k++)
            {
                float d = (x - blob[k].pos) * blob[k].inv;
                float g = 1.0f / (1.0f + d * d);
                w[k] = g * g;
                field += w[k];
            }

            // shared intensity positions every blob's pixel within its own
            // window (faint edges -> hot end); brighter overlaps push toward
            // each window's hot end
            // c (0..1) drives palette position and brightness; intensity past
            // `core` is overshoot. A single blob peaks below core, so it never
            // overshoots — but a strong overlap does, and that blooms the
            // color toward white-hot, so a heavy pileup blows out and then
            // cools back to palette color as the blobs part.
            float raw = field / core;
            float c = raw > 1.0f ? 1.0f : raw;
            float bloom = (raw - 1.0f) * whiteHot;
            if (bloom < 0.0f) bloom = 0.0f;
            if (bloom > 1.0f) bloom = 1.0f;

            float br = minBright + (1.0f - minBright) * motion::ease(c);

            // blend the blobs' colors weighted by their contributions, so an
            // overlap fuses two different palette colors into a new hue
            float fr = 0.0f, fg = 0.0f, fb = 0.0f;
            for (int k = 0; k < n; k++)
            {
                uint8_t r, g, b;
                palette.at(blob[k].base + c * colorSpan, r, g, b);
                fr += w[k] * r;
                fg += w[k] * g;
                fb += w[k] * b;
            }

            // normalise the blend to the average palette color, bloom it
            // toward white by the overshoot, then scale by brightness
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
    // blob k travels at its own constant velocity and only ever reverses at
    // the walls — it never turns around mid-strip. reflect() folds the
    // ever-advancing phase into 0..1 so the blob bounces off each end; pingpong
    // additionally eases the turnaround (by `bounce`) so it isn't a hard flip.
    // The velocities are mutually incommensurate (perturbed by the fractional
    // parts of the golden ratio) so the blobs drift in and out of phase and the
    // whole field never loops, yet each blob's path stays a clean back-and-forth
    // bounce. Phase is measured from t=0 about easedHome, so the strip opens
    // with the blobs uniformly spaced; neighbours start in opposite directions
    // so they diverge at once instead of marching together.
    float blobPos(int k, float t) const
    {
        float g = 0.6180339887f * (k + 1);
        float frac = g - floorf(g);                 // 0..1, irrational walk
        float v = speed * (0.015f + 0.012f * frac); // distinct per-blob speed
        float dir = (k & 1) ? -1.0f : 1.0f;         // alternate start direction
        return motion::pingpong(easedHome[k] + dir * v * t, bounce);
    }

    // inverse of motion::pingpong(., e) on 0..1 by bisection (no closed-form
    // inverse); the blended map h + (ease(h)-h)*e is monotonic in h for
    // e in [0,1]. 24 steps lands within ~1e-7, run once per blob in init()
    static float pingpongInverse(float y, float e)
    {
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 24; i++)
        {
            float m = 0.5f * (lo + hi);
            float f = m + (motion::ease(m) - m) * e;
            if (f < y) lo = m; else hi = m;
        }
        return 0.5f * (lo + hi);
    }

    // a slow, per-blob breathing of the radius so blobs swell and shrink out
    // of step, which makes merges look like they fuse rather than cross
    float blobRadius(int k, float t) const
    {
        float breathe = 1.0f + 0.25f * sinf(t * speed * 0.3f + 1.1f * k);
        return radius * breathe;
    }

    static const int kMax = 32;
    struct Blob { float pos, inv, base; };
    Blob blob[kMax];
    float w[kMax];           // per-blob falloff weights, reused per pixel
    float easedHome[kMax];   // ease^-1 of the uniform start, set in init()

    color::Gradient palette;
    int blobs = 4;
    float radius = 0.14f;
    float speed = 1.0f;
    float bounce = 0.15f;
    float colorSpeed = 1.0f;
    float colorSpan = 0.5f;
    float core = 1.6f;
    float whiteHot = 2.5f;
    float minBright = 0.0f;
};

REGISTER_EFFECT("lava", Lava)
