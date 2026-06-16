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

// recursively sum the regular-file bytes under a directory (0 if it
// doesn't exist or can't be read). lstat, so symlinks aren't followed
inline unsigned long long dirBytes(const std::string& path)
{
    DIR* dir = opendir(path.c_str());
    if (!dir) return 0;

    unsigned long long sum = 0;

    while (dirent* e = readdir(dir))
    {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' ||
             (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;

        std::string child = path + "/" + e->d_name;
        struct stat st;

        if (lstat(child.c_str(), &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
            sum += dirBytes(child);
        else if (S_ISREG(st.st_mode))
            sum += (unsigned long long)st.st_size;
    }

    closedir(dir);
    return sum;
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
// percent integrates live network throughput onto the manifest's byte
// totals (the manifest alone barely moves mid-download -- see the
// percent block below), while "is it downloading right now" comes from
// two signals that must agree:
//   1. the in-flight chunk bytes under steamapps/downloading/ moving --
//      Steam rewrites the manifests only every minute or so (far too
//      coarse to tell a deliberate pause from a slow patch) while the
//      download cache grows continuously during a transfer and freezes
//      the instant it's paused.
//   2. real bytes arriving over the network (netRxBytes).
// requiring both rejects the disk-only churn of a finished download
// being verified/committed: the cache still moves but nothing comes off
// the network, which used to flicker the bar on and off. A deliberate
// pause clears signal 1; verification clears signal 2.
inline const Downloads& downloads()
{
    static Downloads cached;
    static double lastScan = -1e9;
    static double lastCacheMoved = -1e9;
    static double lastNetMoved = -1e9;
    static unsigned long long lastCache = 0;
    static unsigned long long lastRx = 0;
    static double estDone = 0.0;  // bytes pulled, integrated from netRxBytes
    static bool finishHandled = false;  // finish hold already armed this run
    static double finishedUntil = -1e9; // celebration holds until this time

    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;

    if (now - lastScan < 1.0)
        return cached;

    double scanDt = now - lastScan;
    lastScan = now;

    unsigned long long total = 0, done = 0, cache = 0;

    for (const auto& lib : libraries())
    {
        DIR* dir = opendir(lib.c_str());

        if (dir)
        {
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

        cache += dirBytes(lib + "/downloading");
    }

    if (cache != lastCache)
    {
        lastCache = cache;
        lastCacheMoved = now;
    }

    // network throughput is the second authority. A live download runs
    // megabytes/sec; idle background chatter and the disk-only churn of
    // verifying a finished download sit far below this floor, so they
    // don't count as movement. Guard rx > lastRx so a counter reset
    // (interface down/up) reads as zero rather than a huge false spike.
    unsigned long long rx = netRxBytes();

    // bytes off the wire since the last scan -- both the throughput test
    // below and the increment that advances the live percent further down.
    // Guard rx > lastRx so a counter reset (interface down/up) reads as
    // zero rather than a huge false delta.
    double rxDelta = (lastRx != 0 && rx > lastRx) ? (double)(rx - lastRx) : 0.0;

    const double kRxFloor = 50.0 * 1024.0; // bytes/sec

    if (scanDt > 0 && rxDelta / scanDt > kRxFloor)
        lastNetMoved = now;

    lastRx = rx;

    // active requires both authorities within the window: the download
    // cache moving and real bytes arriving. With a 1 s scan a 2 s window
    // tolerates a single dead tick (a brief blip) yet clears ~2 s after
    // a real pause -- about the unpause latency. The cache, not the
    // manifest's flicker-prone StateFlags, decides whether bytes are
    // landing locally, so a stale empty manifest scan can't blank the
    // bar; the network check then rejects the verify/commit phase, where
    // the cache still churns but nothing comes off the wire.
    bool moving = now - lastCacheMoved < 2.0 && now - lastNetMoved < 2.0;

    if (!moving)
    {
        // Activity stopped. If the estimate had essentially reached 100%
        // first, every byte is down and only the local commit remains --
        // treat that as finished and hold a brief active+finished window so
        // the effect can play its completion blink before the rule engine
        // switches away. The network goes quiet the instant a download
        // completes, so without this hold the bar would just vanish at
        // 100% and the done color would never show. Stopping below 100% is
        // a pause/cancel: go straight to inactive.
        const double kFinishHold = 5.0; // seconds to celebrate

        if (!finishHandled && cached.percent >= 99.0f)
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

        // paused / finished-and-celebrated / gone: inactive, no bar. Drop
        // the integrated estimate and re-arm the finish for the next run
        cached = Downloads{};
        estDone = 0.0;
        finishHandled = false;
        finishedUntil = -1e9;
        return cached;
    }

    // still moving: clear any finished state and re-arm so a later
    // completion fires its own hold
    cached.finished = false;
    finishHandled = false;
    finishedUntil = -1e9;

    // Live percent by integrating network throughput. Steam writes the
    // manifest's BytesDownloaded only rarely during a transfer -- often
    // not once across a multi-GB download, flushing it on pause/stop --
    // so done/total alone leaves the bar frozen until you pause. Instead
    // accumulate the bytes coming off the wire: they're in the same
    // compressed units as the manifest counters (plus a little protocol
    // overhead), so each scan's rxDelta is roughly what the manifest would
    // have added had it been flushed.
    //
    // The estimate only ever climbs: the manifest's done is a floor that
    // ratchets it up whenever Steam does flush (and seeds the baseline for
    // a resumed download), and it's capped at total so wire overhead can't
    // push past 100% -- it may reach 100% a little early and sit there
    // until the download finishes and the effect dismisses, which reads
    // better than a bar that jumps backward.
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

            // read only the top-level AppState keys (see addManifest):
            // per-depot blocks repeat the byte counters and a flat read
            // lands on a tiny leftover instead of the real total
            int depth = 0;

            while (fgets(line, sizeof line, f))
            {
                const char* p = line;
                while (*p == ' ' || *p == '\t') p++;

                if (*p == '{') { depth++; continue; }
                if (*p == '}') { depth--; continue; }

                if (depth != 1 || !vdfPair(line, key, val)) continue;

                if (key == "name") name = val;
                else if (key == "StateFlags")
                    flags = strtoul(val.c_str(), nullptr, 10);
                else if (key == "BytesToDownload")
                    toDownload = strtoull(val.c_str(), nullptr, 10);
                else if (key == "BytesDownloaded")
                    downloaded = strtoull(val.c_str(), nullptr, 10);
            }

            fclose(f);

            bool counts = !(toDownload == 0 || (flags & 512) ||
                            !(flags & (256 | 1024)));

            fprintf(out, "  %s \"%s\": StateFlags=%lu BytesToDownload=%llu "
                         "BytesDownloaded=%llu -> %s\n",
                    e->d_name, name.c_str(), flags, toDownload, downloaded,
                    counts ? "COUNTS" : "ignored");

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
