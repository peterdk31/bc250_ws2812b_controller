#pragma once

#include <math.h>
#include <stdint.h>

// The shared "calm flowing" motion primitives. Comet and aurora feel good
// because of a few reusable tricks: ease motion in and out instead of
// snapping, drive fields from several unrelated frequencies so nothing
// visibly loops, and keep a slow brightness shimmer so light is never
// perfectly static. Centralising them here keeps every effect's feel
// consistent and tunable in one place. All are pure functions of
// (position, time) — no per-frame state, no randomness.
namespace motion
{

// smootherstep ease on 0..1: zero slope at both ends, so motion
// accelerates out of rest and decelerates back into it
inline float ease(float x)
{
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

// fold a value into 0..1 by reflection (triangle wave of period 2, running
// 0 -> 1 -> 0). Motion that would leave the 0..1 strip bounces back off each
// end instead of drifting past it.
inline float reflect(float x)
{
    float ph = fmodf(x, 2.0f);
    if (ph < 0) ph += 2.0f;

    return ph < 1.0f ? ph : 2.0f - ph;
}

// triangle wave of period 2 that runs 0 -> 1 -> 0, with the turnarounds
// eased so a bounce decelerates into each end and accelerates away rather
// than reversing instantly. `e` blends linear (0) .. fully eased (1).
inline float pingpong(float t, float e = 1.0f)
{
    float tri = reflect(t);                    // 0..1..0, constant speed
    return tri + (ease(tri) - tri) * e;
}

// the aurora/ember flow field: two sines at unrelated frequencies drifting
// in opposite directions, summed into 0..1. fx/fy are the spatial
// frequencies; with the defaults the pattern never visibly repeats.
inline float flow(float x, float t, float speed = 1.0f,
                  float fx = 5.1f, float fy = 2.3f)
{
    return 0.5f + 0.25f * sinf(x * fx + t * 0.31f * speed)
                + 0.25f * sinf(x * fy - t * 0.17f * speed);
}

// a slow brightness shimmer in 0..1; its frequency and phase are offset
// from flow() so highlights and color don't travel in lockstep
inline float shimmer(float x, float t, float speed = 1.0f, float phase = 1.7f)
{
    return 0.5f + 0.5f * sinf(x * 3.7f + t * 0.23f * speed + phase);
}

// deterministic hash of a 2D integer lattice point -> 0..1
inline float hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFFu) / (float)0x1000000;
}

// smooth 2D value noise at (fx, fy), ~0..1: bilinear blend of the four
// surrounding lattice hashes, smootherstep-eased so there are no creases
inline float valueNoise(float fx, float fy)
{
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float sx = ease(fx - x0), sy = ease(fy - y0);

    float n00 = hash2(x0, y0),     n10 = hash2(x0 + 1, y0);
    float n01 = hash2(x0, y0 + 1), n11 = hash2(x0 + 1, y0 + 1);

    float ix0 = n00 + (n10 - n00) * sx;
    float ix1 = n01 + (n11 - n01) * sx;
    return ix0 + (ix1 - ix0) * sy;
}

// organic flow field in 0..1: 1D value noise drifting in time. A
// less-periodic, more cloud-like alternative to flow(); `scale` sets how
// many noise cells span the strip. Effects blend it with flow() so the
// motion gains texture without losing the calm.
inline float noise(float x, float t, float speed = 1.0f, float scale = 3.0f)
{
    return valueNoise(x * scale, t * 0.15f * speed);
}

// linear blend a..b by m (clamped to 0..1); for mixing flow()/noise()
inline float mix(float a, float b, float m)
{
    if (m < 0) m = 0;
    if (m > 1) m = 1;
    return a + (b - a) * m;
}

} // namespace motion
