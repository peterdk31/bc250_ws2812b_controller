#pragma once

// A fan curve, evaluated the same way on both ends of the wire (README
// "Fans"). The daemon runs the curves whose input only the host can read
// (temperatures, loads, hwmon pwm outputs); the receiver runs the ones whose
// input it reads itself (a PWM signal on one of its GPIOs, source "gpio:N"),
// so those keep following their curve with no daemon and the machine off.
// One evaluator here means the two never disagree about what a curve says.
//
// A curve is 1..MAX_POINTS (x, percent) points sorted by x: linear between
// points, flat beyond the ends, so a curve never has to spell out the x
// axis's 0 or 100. The ramp is the same on both sides too: speed-ups are
// taken at once (cooling first), slow-downs eased at `rate` percent per
// second so a curve never hunts audibly.
namespace fancurve
{
// the most points a curve may have. The receiver stores its curves in a
// fixed-size blob (firmware fan.cpp, tools/fancfg.py) and the phone's editor
// adds no more than this; the daemon holds every curve to it for one rule.
static const int MAX_POINTS = 8;

struct Point
{
    float x, y;
};

// the curve at x; n == 0 is the caller's to avoid (returns 0)
inline float eval(const Point* pts, int n, float x)
{
    if (n <= 0)
        return 0;
    if (x <= pts[0].x)
        return pts[0].y;
    if (x >= pts[n - 1].x)
        return pts[n - 1].y;

    for (int i = 1; i < n; i++)
    {
        if (x <= pts[i].x)
        {
            const Point& a = pts[i - 1];
            const Point& b = pts[i];
            float t = (x - a.x) / (b.x - a.x);
            return a.y + t * (b.y - a.y);
        }
    }

    return pts[n - 1].y;
}

// one ramp step: the output to run this tick given the curve's target, the
// output last tick (`have` false = none yet), the rate in percent per second
// (0 = instant) and the seconds since last tick
inline float ramp(float target, float last, bool have, float rate, float dt)
{
    if (!have || target >= last || rate <= 0)
        return target;

    float out = last - rate * dt;
    return out < target ? target : out;
}
} // namespace fancurve
