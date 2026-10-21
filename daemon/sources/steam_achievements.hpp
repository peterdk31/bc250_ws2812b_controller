#pragma once

#include <stdlib.h>
#include <string>
#include "progress.hpp"
#include "steam_cef.hpp"

// Achievement unlocks, for the steam_achievement condition and the
// achievement effect: Steam's own unlock notification (the toast), counted
// by the debug-port hook (steam_cef.hpp) and timed here on the daemon's
// clock. Progress toasts ("3 / 10") aren't unlocks and don't count.
namespace steam
{

struct Unlock
{
    double at = -1e9;       // progress::mono() when the daemon saw it
    unsigned appid = 0;
    std::string name;
    float global = -1;      // % of players who have it; < 0 unknown
};

class AchievementWatch
{
public:
    // the newest unlock since the daemon started following this Steam
    const Unlock& last()
    {
        double now = progress::mono();
        if (now - lastRead_ < READ_S)
            return last_;
        lastRead_ = now;

        Snapshot snap;
        if (!Cef::get().read(snap))
            return last_;

        const AchievementState& a = snap.achievement;

        // a new hook — the daemon's first look, or Steam restarted — counts
        // from where it stands: unlocks from before aren't replayed
        if (snap.hook != hook_)
        {
            hook_ = snap.hook;
            seq_ = a.seq;
        }
        else if (a.seq != seq_)
        {
            seq_ = a.seq;
            last_ = Unlock{now, a.appid, a.name, a.global};
        }
        return last_;
    }

private:
    static constexpr double READ_S = 0.25;

    Unlock last_;
    double hook_ = 0;
    unsigned seq_ = 0;
    double lastRead_ = -1e9;
};

// STEAM_FAKE_ACHIEVEMENT=N previews without a game: an unlock every N
// seconds (default 10), every other one rare
inline const Unlock* fakeUnlock()
{
    const char* fake = getenv("STEAM_FAKE_ACHIEVEMENT");
    if (!fake || !*fake)
        return nullptr;

    double every = atof(fake);
    if (every <= 0) every = 10;

    static double start = progress::mono();
    static Unlock sim;

    long n = (long)((progress::mono() - start) / every);
    sim.at = start + n * every;
    sim.name = "Fake achievement";
    sim.global = n % 2 ? 2.0f : 45.0f;
    return &sim;
}

inline const Unlock& lastUnlock()
{
    if (const Unlock* sim = fakeUnlock())
        return *sim;

    static AchievementWatch watch;
    return watch.last();
}

// seconds since the newest unlock; huge when there hasn't been one
inline double sinceUnlock()
{
    return progress::mono() - lastUnlock().at;
}

} // namespace steam
