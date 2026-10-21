#pragma once

#include <algorithm>
#include <string>
#include "progress.hpp"
#include "steam_cef.hpp"

// A game launch's progress, the steam_launch progress source: the counted
// steps of Steam's launch dialog — processing Vulkan shaders above all, also
// verifying files or fetching workshop items — read from the launch's game
// action over the debug port (steam_cef.hpp). `active` = a task with counts
// is underway; steps without counts (syncing the cloud, creating the
// process) show nothing.
namespace steam
{

class LaunchTracker
{
public:
    const progress::Reading& update()
    {
        double now = progress::mono();

        // the rule tick and the per-frame effect share one look; Steam's
        // numbers move once a second at most
        if (now - lastRead_ < READ_S)
            return cached_;
        lastRead_ = now;

        Snapshot snap;
        bool fresh = Cef::get().read(snap);
        const LaunchState& l = snap.launch;

        bool counting = fresh && l.appid && l.total > 0 && l.done < l.total;

        if (counting)
        {
            if (l.appid != appid_ || l.task != task_)
                peak_ = 0;
            appid_ = l.appid;
            task_ = l.task;
            peak_ = std::max(peak_, (float)(100.0 * l.done / l.total));

            cached_ = progress::Reading{true, false, peak_};
            finishedUntil_ = -1e9;
            return cached_;
        }

        // the task ended. Its last reading lags the real end by up to a
        // poll, so near-full counts as done; a skip or a cancel doesn't
        bool failed = l.task == "Cancelled" || l.task == "Failed";
        if (cached_.active && !cached_.finished)
        {
            bool done = fresh && l.appid == appid_ && l.total > 0 && l.done >= l.total;
            if (done || (fresh && !failed && peak_ >= DONE_PERCENT))
                finishedUntil_ = now + FINISH_HOLD_S;
            appid_ = 0;
            task_.clear();
            peak_ = 0;
        }

        cached_ = now < finishedUntil_ ? progress::Reading{true, true, 100.0f}
                                       : progress::Reading{};
        return cached_;
    }

private:
    static constexpr double FINISH_HOLD_S = 3.0;
    static constexpr float DONE_PERCENT = 90.0f;
    static constexpr double READ_S = 0.25;

    progress::Reading cached_;
    double lastRead_ = -1e9;
    unsigned appid_ = 0;
    std::string task_;
    float peak_ = 0;            // counts only climb within one task
    double finishedUntil_ = -1e9;
};

// STEAM_FAKE_LAUNCH previews the bar without a launch (progress::Fake)
inline const progress::Reading& launch()
{
    static progress::Fake fake("STEAM_FAKE_LAUNCH");
    if (const progress::Reading* sim = fake.read())
        return *sim;

    static LaunchTracker tracker;
    return tracker.update();
}

} // namespace steam
