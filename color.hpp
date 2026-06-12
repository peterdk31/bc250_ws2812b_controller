#pragma once

#include <math.h>
#include <stdint.h>

// RRGGBB ↔ HSV helpers and a two-color ramp, shared by effects that
// build gradients from configurable colors
namespace color
{

struct Hsv
{
    float h = 0, s = 0, v = 0;
};

inline Hsv toHsv(uint32_t rgb)
{
    float r = ((rgb >> 16) & 0xFF) / 255.0f;
    float g = ((rgb >> 8) & 0xFF) / 255.0f;
    float b = (rgb & 0xFF) / 255.0f;

    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;

    Hsv out;

    out.v = mx;
    out.s = mx > 0 ? d / mx : 0;

    // gray has no hue; leave it at 0
    if (d <= 0)
        return out;

    if (mx == r)
        out.h = fmodf((g - b) / d, 6.0f) / 6.0f;
    else if (mx == g)
        out.h = ((b - r) / d + 2.0f) / 6.0f;
    else
        out.h = ((r - g) / d + 4.0f) / 6.0f;

    if (out.h < 0)
        out.h += 1.0f;

    return out;
}

inline void toRgb(float h, float s, float v,
                  uint8_t& r, uint8_t& g, uint8_t& b)
{
    h = fmodf(h, 1.0f);
    if (h < 0) h += 1.0f;

    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h * 6, 2) - 1));
    float m = v - c;

    float rp, gp, bp;

    switch ((int)(h * 6))
    {
        case 0: rp = c; gp = x; bp = 0; break;
        case 1: rp = x; gp = c; bp = 0; break;
        case 2: rp = 0; gp = c; bp = x; break;
        case 3: rp = 0; gp = x; bp = c; break;
        case 4: rp = x; gp = 0; bp = c; break;
        default: rp = c; gp = 0; bp = x; break;
    }

    r = (uint8_t)((rp + m) * 255);
    g = (uint8_t)((gp + m) * 255);
    b = (uint8_t)((bp + m) * 255);
}

// two-color gradient that walks the hue wheel downward from `from` to
// `to`, so blue→red passes through green and yellow (the thermal
// look), not through magenta; saturation and value blend linearly
class Ramp
{
public:
    Ramp() : Ramp(0x0000ff, 0xff0000) {}

    Ramp(uint32_t from, uint32_t to)
        : a(toHsv(from)), b(toHsv(to))
    {
        dh = a.h - b.h;
        if (dh < 0) dh += 1.0f;
    }

    // x 0 (`from`) .. 1 (`to`)
    void at(float x, uint8_t& r, uint8_t& g, uint8_t& bl) const
    {
        if (x < 0) x = 0;
        if (x > 1) x = 1;

        toRgb(a.h - dh * x,
              a.s + (b.s - a.s) * x,
              a.v + (b.v - a.v) * x,
              r, g, bl);
    }

private:
    Hsv a, b;
    float dh = 0;
};

} // namespace color
