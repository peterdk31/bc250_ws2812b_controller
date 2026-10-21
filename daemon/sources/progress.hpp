#pragma once

#include <stdlib.h>
#include <string.h>
#include <time.h>

// What the progress effect draws and the progress conditions test: one
// bar's state, whatever fills it. Each source (progress_sources.hpp) turns
// its own data into this shape.
namespace progress
{

struct Reading
{
    bool active = false;    // the bar is showing: running, or celebrating
    bool finished = false;  // just completed -- the effect celebrates, then ends
    float percent = 0.0f;   // 0..100
};

inline double mono()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// A source's preview without the real thing, from an environment variable:
// a number is a static bar, "ramp[N]" climbs N %/s (default 3) in 1 s steps
// like a real scan, then celebrates. Null when the variable is unset.
class Fake
{
public:
    explicit Fake(const char* env) : env_(env) {}

    const Reading* read()
    {
        const char* fake = getenv(env_);
        if (!fake || !*fake)
            return nullptr;

        double now = mono();
        if (start_ < 0)
            start_ = now;

        sim_.active = true;

        if (strncmp(fake, "ramp", 4) == 0)
        {
            long rate = atoi(fake + 4);
            if (rate <= 0) rate = 3;

            long fill = (100 + rate - 1) / rate;
            long cycle = fill + 4;               // + the celebration
            long s = (long)(now - start_) % cycle;
            long pct = s * rate;

            sim_.finished = pct >= 100;
            sim_.percent = (float)(pct >= 100 ? 100 : pct);
        }
        else
        {
            sim_.finished = false;
            sim_.percent = (float)atof(fake);
        }

        return &sim_;
    }

private:
    const char* env_;
    Reading sim_;
    double start_ = -1;
};

} // namespace progress
