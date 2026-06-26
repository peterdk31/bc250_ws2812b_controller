#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <vector>

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

// a palette of evenly-spaced color stops, sampled 0..1 with linear RGB
// interpolation between adjacent stops. Built from a spec string of hex
// colors separated by anything non-hex (comma/space/#), e.g.
// "2a0a00, ff7d1e, ffd060". Linear RGB stays clean as long as adjacent
// stops are near each other in hue — for a vivid multi-hue sweep add the
// intermediate colors as stops rather than relying on one long blend
// (a single red→green blend would pass through mud; red,yellow,green
// won't). Unlike Ramp it never walks the hue wheel, so it can't wrap the
// long way around. Works on both backends (host + receiver).
class Gradient
{
public:
    Gradient() { add(0xffffff); }

    explicit Gradient(const std::string& spec)
    {
        const char* s = spec.c_str();

        while (*s)
        {
            while (*s && !isHex(*s)) s++;     // skip separators
            if (!*s) break;

            char* end;
            unsigned long c = strtoul(s, &end, 16);
            if (end == s) break;              // no progress; bail

            add((uint32_t)c);
            s = end;
        }

        if (r.empty()) add(0xffffff);
    }

    int stops() const { return (int)r.size(); }

    // x 0 (first stop) .. 1 (last stop)
    void at(float x, uint8_t& ro, uint8_t& go, uint8_t& bo) const
    {
        int n = (int)r.size();

        if (n == 1)
        {
            ro = (uint8_t)r[0]; go = (uint8_t)g[0]; bo = (uint8_t)b[0];
            return;
        }

        if (x < 0) x = 0;
        if (x > 1) x = 1;

        float p = x * (n - 1);
        int i = (int)p;
        if (i >= n - 1) i = n - 2;
        float f = p - i;

        ro = (uint8_t)(r[i] + (r[i + 1] - r[i]) * f);
        go = (uint8_t)(g[i] + (g[i + 1] - g[i]) * f);
        bo = (uint8_t)(b[i] + (b[i + 1] - b[i]) * f);
    }

private:
    std::vector<float> r, g, b;

    static bool isHex(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
            || (c >= 'A' && c <= 'F');
    }

    void add(uint32_t c)
    {
        r.push_back((c >> 16) & 0xFF);
        g.push_back((c >> 8) & 0xFF);
        b.push_back(c & 0xFF);
    }
};

} // namespace color
