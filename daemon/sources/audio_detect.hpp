#pragma once

#include <stdio.h>
#include <string>
#include "audio.hpp"

namespace audio
{

// Decides "is system audio actually playing right now", wrapping the
// detection mechanics so callers (the audio_playing rule condition)
// only deal with a bool.
//
// The answer comes from the user session's sound server: its playback
// stream list (StreamQuery, spawned on one call and harvested on a
// later one so callers never block) says uncorked sink-inputs are real
// playback and corked/none is paused/stopped — unconfused by how quiet
// the music is or by our own capture holding the sink awake.
//
// Queries only run while the cheap /proc/asound RUNNING flag is up (or
// just was): when it has been down for a while the sink is suspended,
// nothing can be playing, and a fully idle system costs one procfs
// read per call.
//
// There is no plan B: when no verdict can be had (no user session, no
// pactl, unreadable output) audio counts as not playing, and the first
// such failure while the flag says something is audible leaves a note
// on stderr so a broken setup is visible instead of silently dark.
//
// One instance per interested caller (it owns child-process state);
// call playing() once per rule tick with a monotonic timestamp.
class Detector
{
public:
    bool playing(double now)
    {
        bool run = alsaRunning();

        if (run)
            lastRun = now;

        // the tail keeps verdicts flowing across brief flag dips
        // (sink suspend/resume edges)
        bool care = run || (lastRun >= 0 && now - lastRun < 10.0);

        if (query.running())
        {
            std::string doc;
            int done = query.poll(doc);

            if (done == 1)
            {
                truth = classifySinkInputs(doc);
                truthAt = now;

                if (truth == Streams::UNKNOWN)
                    complain(run, "audio detection: pactl output unreadable");
            }
            else if (done == 0 || now - queryAt > 2.5)
            {
                if (done < 0)
                    query.abandon(); // hung child

                truth = Streams::UNKNOWN;
                truthAt = now;
                complain(run, "audio detection: pactl query failed");
            }
        }

        if (care && !query.running() && now - queryAt >= 1.0)
        {
            queryAt = now; // also rate-limits retries when pactl is absent

            if (!query.start())
            {
                truth = Streams::UNKNOWN;
                truthAt = now;
                complain(run, "audio detection: pactl not found");
            }
        }

        // only a fresh PLAYING verdict counts; queries refresh it well
        // within its shelf life while anything is audible
        return truth == Streams::PLAYING && now - truthAt < 3.0;
    }

private:
    // once, on the first failure that matters: the flag says something
    // is audible but the sound server can't be asked, so audio_playing
    // stays false until queries work
    void complain(bool audible, const char* what)
    {
        if (warned || !audible)
            return;

        fprintf(stderr, "%s; audio_playing stays false until the sound "
                        "server's stream list is readable\n", what);
        warned = true;
    }

    double lastRun = -1;
    StreamQuery query;
    double queryAt = -1e9;
    Streams truth = Streams::UNKNOWN;
    double truthAt = -1;
    bool warned = false;
};

} // namespace audio
