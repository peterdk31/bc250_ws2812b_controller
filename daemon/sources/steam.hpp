#pragma once

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>

// Steam download tracking by polling steamapps/appmanifest_*.acf:
// Steam rewrites the manifests as chunks land, so their byte counters
// are the standard way to watch downloads from outside. Shared by the
// steam_dl condition and the steam_download effect
namespace steam
{

struct Downloads
{
    bool active = false;    // a download is running and bytes are moving
    bool finished = false;  // just completed -- effect celebrates, then ends
    float percent = 0.0f;   // 0..100 across all active downloads
};

// `"key"  "value"` out of a VDF/ACF line; false for any other shape
inline bool vdfPair(const char* line, std::string& key, std::string& val)
{
    const char* a = strchr(line, '"');
    if (!a) return false;

    const char* b = strchr(a + 1, '"');
    if (!b) return false;

    const char* c = strchr(b + 1, '"');
    if (!c) return false;

    const char* d = strchr(c + 1, '"');
    if (!d) return false;

    key.assign(a + 1, b - a - 1);
    val.assign(c + 1, d - c - 1);
    return true;
}

inline void addLibrary(std::vector<std::string>& libs, const std::string& dir)
{
    char real[PATH_MAX];

    // also rejects directories that don't exist
    if (!realpath(dir.c_str(), real))
        return;

    if (std::find(libs.begin(), libs.end(), real) == libs.end())
        libs.push_back(real);
}

// every steamapps dir on the system: per-user Steam roots under /home
// and ostree's /var/home (realpath collapses the /home symlink and the
// .steam/steam indirection, so nothing is scanned twice), plus the
// extra libraries each root's libraryfolders.vdf points at (SD card,
// second disk)
inline std::vector<std::string> libraries()
{
    std::vector<std::string> libs;

    for (const char* root : {"/home", "/var/home"})
    {
        DIR* dir = opendir(root);
        if (!dir) continue;

        while (dirent* e = readdir(dir))
        {
            if (e->d_name[0] == '.')
                continue;

            std::string home = std::string(root) + "/" + e->d_name;

            addLibrary(libs, home + "/.local/share/Steam/steamapps");
            addLibrary(libs, home + "/.steam/steam/steamapps");
        }

        closedir(dir);
    }

    size_t roots = libs.size();

    for (size_t i = 0; i < roots; i++)
    {
        FILE* f = fopen((libs[i] + "/libraryfolders.vdf").c_str(), "r");
        if (!f) continue;

        char line[PATH_MAX + 64];
        std::string key, val;

        while (fgets(line, sizeof line, f))
            if (vdfPair(line, key, val) && key == "path")
                addLibrary(libs, val + "/steamapps");

        fclose(f);
    }

    return libs;
}

// fold one manifest's outstanding bytes into the totals. StateFlags
// bits (community-documented): 2 update required, 256 update running,
// 512 update paused, 1024 update started. Requiring 256|1024 rather
// than just 2 skips games whose update is pending but not downloading
// ("update on next launch" sits at StateFlags 6 indefinitely)
inline void addManifest(const std::string& path,
                        unsigned long long& total,
                        unsigned long long& done)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;

    unsigned long flags = 0;
    unsigned long long toDownload = 0, downloaded = 0;

    char line[512];
    std::string key, val;

    // the appmanifest is nested VDF. The top-level AppState block holds
    // the whole-update StateFlags and byte totals, but blocks deeper in
    // (per-depot download state) repeat BytesToDownload/BytesDownloaded
    // for their own slice -- a live download can carry a dozen-plus, the
    // last a tiny 240-byte depot. A flat last-match read lands on that
    // leftover instead of the real total, so track brace depth and read
    // only the depth-1 AppState keys. Braces sit alone on their own lines.
    int depth = 0;

    while (fgets(line, sizeof line, f))
    {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '{') { depth++; continue; }
        if (*p == '}') { depth--; continue; }

        if (depth != 1 || !vdfPair(line, key, val))
            continue;

        if (key == "StateFlags")
            flags = strtoul(val.c_str(), nullptr, 10);
        else if (key == "BytesToDownload")
            toDownload = strtoull(val.c_str(), nullptr, 10);
        else if (key == "BytesDownloaded")
            downloaded = strtoull(val.c_str(), nullptr, 10);
    }

    fclose(f);

    if (toDownload == 0 || (flags & 512) || !(flags & (256 | 1024)))
        return;

    total += toDownload;
    done += downloaded;
}

// total bytes received across every interface except loopback, from
// /proc/net/dev. Used to tell a live network download from disk-only
// churn (Steam verifying/committing a finished download still rewrites
// the downloading/ cache, but pulls nothing over the network)
inline unsigned long long netRxBytes()
{
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f) return 0;

    char line[512];
    unsigned long long sum = 0;

    // each data line is "  iface: rxbytes rxpackets ..."; the two header
    // lines carry no colon so they're skipped
    while (fgets(line, sizeof line, f))
    {
        char* colon = strchr(line, ':');
        if (!colon) continue;

        char* name = line;
        while (*name == ' ' || *name == '\t') name++;
        *colon = '\0';

        if (strcmp(name, "lo") == 0)
            continue;

        sum += strtoull(colon + 1, nullptr, 10);
    }

    fclose(f);
    return sum;
}

// state of all Steam downloads, rescanned at most every 1 s so the
// 0.5 s rule tick and the per-frame effect share one scan's cost.
//
// "is a download in progress" comes from the manifests: addManifest sums
// BytesToDownload/BytesDownloaded only for manifests Steam marks as
// actively updating (running, not paused, bytes outstanding), so total>0
// means a transfer is underway. That manifest flag stays set across
// EVERY part of a multi-part install, where the older download-cache test
// went quiet between parts and dropped the effect on the early ones.
//
// "is it transferring right now" (cached.active, what steam_dl keys on)
// adds a network check: real bytes arriving over the wire. That rejects a
// deliberate pause and the disk-only churn of verifying a finished
// download (the manifest may still read "running", but nothing comes off
// the network). The window is wide enough that an ordinary dip between
// parts doesn't flap the effect off.
//
// percent integrates network throughput onto the manifest's byte totals
// (the manifest's BytesDownloaded barely moves mid-download -- Steam
// flushes it on pause/stop -- see the percent block below). Each part
// carries its own BytesToDownload; when it changes the bar restarts at 0
// for the new part. The percent is held in cached across brief
// inactivity, so an effect switching away and back resumes where it was
// instead of snapping to 0.
// preview/dev override (no real download needed): set STEAM_FAKE_PERCENT to
// a number for a static active bar, or to "ramp" to simulate a download that
// climbs ~3%/s and celebrates on completion. The percent is quantized to 1 s
// steps so the bumps land like the real 1 Hz manifest scan, which is what the
// steam_download effect's surge reacts to. Inert when the env var is unset.
inline const Downloads* fakeDownloads()
{
    const char* fake = getenv("STEAM_FAKE_PERCENT");
    if (!fake || !*fake)
        return nullptr;

    static Downloads sim;
    static double start = -1e9;

    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;

    if (start < 0)
        start = now;

    if (strncmp(fake, "ramp", 4) == 0)
    {
        long rate = atoi(fake + 4);       // "ramp6" -> 6 %/s; "ramp" -> default
        if (rate <= 0) rate = 3;

        long fill = (100 + rate - 1) / rate; // seconds to reach 100%
        long cycle = fill + 4;               // + a brief finish/celebration
        long s = (long)(now - start) % cycle;
        long pct = s * rate;                 // already in 1 Hz steps

        sim.finished = pct >= 100;
        sim.active = true;
        sim.percent = (float)(pct >= 100 ? 100 : pct);
    }
    else
    {
        sim.active = true;
        sim.finished = false;
        sim.percent = (float)atof(fake);
    }

    return &sim;
}

inline const Downloads& downloads()
{
    if (const Downloads* sim = fakeDownloads())
        return *sim;

    static Downloads cached;
    static double lastScan = -1e9;
    static double lastNetMoved = -1e9;
    static unsigned long long lastRx = 0;
    static double estDone = 0.0;  // bytes pulled, integrated from netRxBytes
    static unsigned long long lastTarget = 0; // BytesToDownload we're tracking
    static double idleSince = -1e9;     // when a running manifest last vanished
    static bool finishHandled = false;  // finish hold already armed this run
    static double finishedUntil = -1e9; // celebration holds until this time

    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;

    if (now - lastScan < 1.0)
        return cached;

    double scanDt = now - lastScan;
    lastScan = now;

    unsigned long long total = 0, done = 0;

    for (const auto& lib : libraries())
    {
        DIR* dir = opendir(lib.c_str());
        if (!dir) continue;

        while (dirent* e = readdir(dir))
        {
            size_t len = strlen(e->d_name);

            if (strncmp(e->d_name, "appmanifest_", 12) != 0 ||
                len < 4 || strcmp(e->d_name + len - 4, ".acf") != 0)
                continue;

            addManifest(lib + "/" + e->d_name, total, done);
        }

        closedir(dir);
    }

    // addManifest only counts running, non-paused manifests with bytes
    // outstanding, so total>0 is a stable "Steam is downloading something"
    // that holds across every part of the job
    bool present = total > 0;

    // network throughput is the second authority. A live download runs
    // megabytes/sec; idle background chatter and the disk-only churn of
    // verifying a finished download sit far below this floor. Guard
    // rx > lastRx so a counter reset (interface down/up) reads as zero
    // rather than a huge false delta.
    unsigned long long rx = netRxBytes();
    double rxDelta = (lastRx != 0 && rx > lastRx) ? (double)(rx - lastRx) : 0.0;
    lastRx = rx;

    const double kRxFloor = 50.0 * 1024.0; // bytes/sec

    if (scanDt > 0 && rxDelta / scanDt > kRxFloor)
        lastNetMoved = now;

    // active = a manifest says a download is underway AND bytes are
    // arriving. The window tolerates an ordinary dip between parts without
    // flapping the effect off, yet a real pause or the verify phase
    // (network goes silent) clears it within a few seconds -- long before
    // Steam's minute-coarse paused flag would.
    const double kNetGrace = 10.0; // seconds of net-quiet before inactive
    bool active = present && (now - lastNetMoved < kNetGrace);

    // a game install is really several apps downloaded one after another
    // -- the game, Steamworks Common Redistributables (appid 228980),
    // Proton, shader pre-cache -- each its own appmanifest. Steam runs them
    // sequentially, so total tracks whichever is running now; when it
    // finishes and the next starts, total changes, so restart the bar at 0
    // (seeded from this part's already-downloaded floor, e.g. a resumed
    // part) and let it fill again. Per-depot progress WITHIN one app isn't
    // exposed anywhere outside Steam, so per-app is as granular as it gets.
    if (present && total != lastTarget)
    {
        lastTarget = total;
        estDone = (double)done;
    }

    if (active)
    {
        idleSince = -1e9;
        finishHandled = false;
        finishedUntil = -1e9;

        // Live percent by integrating network throughput. Steam writes the
        // manifest's BytesDownloaded only rarely during a transfer --
        // often not once across a multi-GB part, flushing it on
        // pause/stop -- so done/total alone leaves the bar frozen until you
        // pause. Instead accumulate the bytes coming off the wire: they're
        // in the same compressed units as the manifest counters (plus a
        // little protocol overhead). The estimate only ever climbs: the
        // manifest's done is a floor that ratchets it up whenever Steam
        // does flush, and it's capped at total so wire overhead can't push
        // past 100%.
        estDone += rxDelta;

        if ((double)done > estDone)
            estDone = (double)done;

        if (total > 0)
        {
            if (estDone > (double)total)
                estDone = (double)total;

            cached.percent = 100.0f * (float)(estDone / (double)total);
        }

        cached.active = true;
        cached.finished = false;
        return cached;
    }

    // not transferring: pause / verify / finishing / gone / transient dip
    if (idleSince < 0.0)
        idleSince = now;

    // a finish is when NO manifest is running any more (present==false)
    // and the estimate had essentially reached 100%: every byte is down
    // and only the local commit remains. Hold a brief active+finished
    // window so the effect can play its completion blink before the rule
    // engine switches away (the network goes quiet the instant a download
    // ends). Gating on !present is what stops a part boundary -- where one
    // part hits 100% but the next is still running -- from celebrating
    // mid-job.
    const double kFinishHold = 5.0; // seconds to celebrate

    if (!finishHandled && !present && cached.percent >= 99.0f && lastTarget > 0)
    {
        finishedUntil = now + kFinishHold;
        finishHandled = true;
    }

    if (now < finishedUntil)
    {
        cached.active = true;
        cached.finished = true;
        cached.percent = 100.0f;
        return cached;
    }

    // forget the download once its celebration is over, or once no running
    // manifest has been seen for long enough that a stale percent
    // shouldn't bleed into the next one. A brief stall or a quick effect
    // switch keeps the percent (present stays true, or we're inside the
    // grace), so re-activating resumes smoothly instead of from 0 -- which
    // is what made a download that briefly lost the effect snap back to 0%.
    const double kResetGrace = 30.0; // seconds

    if ((finishHandled && now >= finishedUntil) ||
        (!present && now - idleSince > kResetGrace))
    {
        estDone = 0.0;
        lastTarget = 0;
        finishHandled = false;
        finishedUntil = -1e9;
        cached = Downloads{};
        return cached;
    }

    // inactive but remembered: report not transferring, keep the percent
    cached.active = false;
    cached.finished = false;
    return cached;
}

// one verbose scan for debugging detection (the daemon's `--steam-status`):
// prints every library found, every appmanifest with its StateFlags and
// byte counters, whether addManifest's filter would count it as an active
// download (and if not, why), and the resulting percent. Mirrors the
// libraries()/addManifest logic so what it reports is what downloads() acts on
inline void dumpStatus(FILE* out)
{
    std::vector<std::string> libs = libraries();

    fprintf(out, "steam libraries found: %zu\n", libs.size());

    for (const auto& l : libs)
        fprintf(out, "  %s\n", l.c_str());

    if (libs.empty())
        fprintf(out, "  (none matched — Steam may be installed somewhere "
                     "libraries() doesn't scan, e.g. a Flatpak under "
                     "~/.var/app or a Snap under ~/snap)\n");

    unsigned long long total = 0, done = 0;
    int manifests = 0, counted = 0;

    for (const auto& lib : libs)
    {
        DIR* dir = opendir(lib.c_str());
        if (!dir) continue;

        while (dirent* e = readdir(dir))
        {
            size_t len = strlen(e->d_name);

            if (strncmp(e->d_name, "appmanifest_", 12) != 0 ||
                len < 4 || strcmp(e->d_name + len - 4, ".acf") != 0)
                continue;

            manifests++;

            FILE* f = fopen((lib + "/" + e->d_name).c_str(), "r");
            if (!f) { fprintf(out, "  %s: cannot open\n", e->d_name); continue; }

            unsigned long flags = 0;
            unsigned long long toDownload = 0, downloaded = 0;
            std::string name, key, val;
            char line[512];

            // downloads() reads only the top-level (depth-1) AppState keys
            // (per-depot blocks repeat the byte counters and a flat read
            // lands on a tiny leftover instead of the real total). But the
            // deeper per-depot BytesTo/Downloaded blocks are exactly what a
            // future per-part bar would need, and they only exist while a
            // download is live -- so dump them here too, with their depth,
            // to capture the structure from a real multi-part download.
            std::vector<std::string> deeper;
            int depth = 0;

            while (fgets(line, sizeof line, f))
            {
                const char* p = line;
                while (*p == ' ' || *p == '\t') p++;

                if (*p == '{') { depth++; continue; }
                if (*p == '}') { depth--; continue; }

                if (!vdfPair(line, key, val)) continue;

                if (depth == 1)
                {
                    if (key == "name") name = val;
                    else if (key == "StateFlags")
                        flags = strtoul(val.c_str(), nullptr, 10);
                    else if (key == "BytesToDownload")
                        toDownload = strtoull(val.c_str(), nullptr, 10);
                    else if (key == "BytesDownloaded")
                        downloaded = strtoull(val.c_str(), nullptr, 10);
                }
                else if (depth > 1 &&
                         (key == "BytesToDownload" || key == "BytesDownloaded"))
                {
                    deeper.push_back("depth " + std::to_string(depth) + " " +
                                     key + "=" + val);
                }
            }

            fclose(f);

            bool counts = !(toDownload == 0 || (flags & 512) ||
                            !(flags & (256 | 1024)));

            fprintf(out, "  %s \"%s\": StateFlags=%lu BytesToDownload=%llu "
                         "BytesDownloaded=%llu -> %s\n",
                    e->d_name, name.c_str(), flags, toDownload, downloaded,
                    counts ? "COUNTS" : "ignored");

            for (const auto& d : deeper)
                fprintf(out, "      per-depot: %s\n", d.c_str());

            if (!counts)
            {
                if (toDownload == 0)
                    fprintf(out, "      reason: BytesToDownload is 0\n");
                if (flags & 512)
                    fprintf(out, "      reason: paused (StateFlags bit 512)\n");
                if (!(flags & (256 | 1024)))
                    fprintf(out, "      reason: no running bit 256/1024 in "
                                 "StateFlags\n");
            }
            else { total += toDownload; done += downloaded; counted++; }
        }

        closedir(dir);
    }

    fprintf(out, "manifests scanned: %d, counted as active: %d\n",
            manifests, counted);

    if (total > 0)
        fprintf(out, "totals: %llu / %llu bytes -> %.1f%% (manifest baseline; "
                     "the live bar advances this with network RX between the "
                     "manifest's infrequent flushes)\n",
                done, total, 100.0 * done / total);
    else
        fprintf(out, "totals: nothing counted -> percent 0\n");

    // sample network RX over ~1 s: active needs real throughput, not just
    // a manifest that counts, so report the rate and how it compares to
    // the floor that separates a live download from verify/idle chatter
    const double kRxFloor = 50.0 * 1024.0;

    unsigned long long rx0 = netRxBytes();
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    timespec nap{1, 0};
    nanosleep(&nap, nullptr);

    unsigned long long rx1 = netRxBytes();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double rate = (dt > 0 && rx1 > rx0) ? (rx1 - rx0) / dt : 0.0;
    bool netActive = rate > kRxFloor;

    fprintf(out, "network RX: %.0f KiB/s (floor %.0f KiB/s) -> %s\n",
            rate / 1024.0, kRxFloor / 1024.0,
            netActive ? "downloading" : "idle/verify");

    fprintf(out, "steam_dl %s\n",
            (counted > 0 && netActive)
                ? "would be active (manifest counts + network flowing)"
                : "inactive (needs a counted manifest AND network throughput; "
                  "a finished download being verified counts but sends nothing)");
}

} // namespace steam
