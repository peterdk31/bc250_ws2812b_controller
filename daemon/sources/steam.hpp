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
#include "steam_cef.hpp"

// Steam download progress for the steam_dl condition and the steam_download
// effect: Steam's own numbers over its debug port when available
// (steam_cef.hpp), otherwise an estimate from the appmanifests
namespace steam
{

struct Downloads
{
    bool active = false;    // a download is running and bytes are moving
    bool finished = false;  // just completed -- effect celebrates, then ends
    float percent = 0.0f;   // 0..100 for the part downloading now
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

// every steamapps dir: each user's Steam root under /home and ostree's
// /var/home, plus the extra libraries its libraryfolders.vdf lists.
// realpath dedupes the /home symlink and the .steam/steam indirection
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

struct Manifest
{
    std::string name;
    unsigned long flags = 0;
    unsigned long long toDownload = 0, downloaded = 0;
    std::vector<std::string> depots; // per-depot counters, for --steam-status

    // StateFlags (community-documented): 2 update required, 256 running,
    // 512 paused, 1024 started. 2 alone is "update on next launch", which
    // can sit there indefinitely
    bool running() const
    {
        return toDownload > 0 && !(flags & 512) && (flags & (256 | 1024));
    }
};

inline bool readManifest(const std::string& path, Manifest& m, bool depots = false)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;

    char line[512];
    std::string key, val;

    // per-depot blocks nested inside AppState repeat BytesToDownload/
    // BytesDownloaded for their own slice, so only depth-1 keys are the
    // app's totals. Braces sit alone on their lines
    int depth = 0;

    while (fgets(line, sizeof line, f))
    {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '{') { depth++; continue; }
        if (*p == '}') { depth--; continue; }

        if (!vdfPair(line, key, val))
            continue;

        if (depth == 1)
        {
            if (key == "name") m.name = val;
            else if (key == "StateFlags")
                m.flags = strtoul(val.c_str(), nullptr, 10);
            else if (key == "BytesToDownload")
                m.toDownload = strtoull(val.c_str(), nullptr, 10);
            else if (key == "BytesDownloaded")
                m.downloaded = strtoull(val.c_str(), nullptr, 10);
        }
        else if (depots && depth > 1 &&
                 (key == "BytesToDownload" || key == "BytesDownloaded"))
        {
            m.depots.push_back("depth " + std::to_string(depth) + " " + key + "=" + val);
        }
    }

    fclose(f);
    return true;
}

// f(file name, path) for every appmanifest_*.acf in the libraries
template <class F>
inline void forEachManifest(const std::vector<std::string>& libs, F f)
{
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

            f(e->d_name, lib + "/" + e->d_name);
        }

        closedir(dir);
    }
}

// bytes received on every interface but loopback
inline unsigned long long netRxBytes()
{
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f) return 0;

    char line[512];
    unsigned long long sum = 0;

    // "  iface: rxbytes ..."; the header lines have no colon
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

// STEAM_FAKE_PERCENT previews the effect without a download: a number is a
// static bar, "ramp[N]" climbs N %/s (default 3) in 1 s steps like the real
// scan, then celebrates
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
        long rate = atoi(fake + 4);
        if (rate <= 0) rate = 3;

        long fill = (100 + rate - 1) / rate;
        long cycle = fill + 4;               // + the celebration
        long s = (long)(now - start) % cycle;
        long pct = s * rate;

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

// downloads run MB/s; background chatter and post-download verification don't
constexpr double RX_FLOOR = 50.0 * 1024.0; // bytes/sec

// one look at Steam's downloads, from either source
struct Sample
{
    bool present = false;          // a download is underway, not paused
    bool active = false;           // ...and progressing right now
    unsigned long long part = 0;   // the app (or part) downloading now
    float percent = 0.0f;
    bool jobDone = false;          // with !present: it ended complete, not paused or abandoned
};

// `last` is the last sample that had a download present; false when Steam
// has no fresh reading
inline bool steamSample(Sample& s, const Sample& last)
{
    Live live;
    if (!Cef::get().read(live))
        return false;

    s.present = s.active = live.running;
    s.part = live.appid;
    s.percent = live.percent;

    // the last reading before a finish can sit short of 100%; and a part
    // completing into a paused or waiting next one isn't the end of the job
    s.jobDone = !live.running && live.queued == 0 &&
                (live.isCompleted((unsigned)last.part) || last.percent >= 99.0f);
    return true;
}

// The manifests mark a running download reliably, but Steam only flushes
// BytesDownloaded on pause/stop, so the percent integrates network RX onto
// the manifest's total instead, with its BytesDownloaded as a floor. The
// network also tells a real transfer from a pause or the verify phase,
// which the manifest flags show minutes late.
class ManifestEstimate
{
public:
    // carry: the percent Steam's own reading had reached when it dropped
    // out mid-part (< 0 if it didn't), so the bar doesn't jump back
    Sample sample(double now, double dt, double rxDelta, const Sample& last, float carry)
    {
        unsigned long long total = 0, done = 0;

        forEachManifest(libraries(), [&](const char*, const std::string& path) {
            Manifest m;
            if (readManifest(path, m) && m.running())
            {
                total += m.toDownload;
                done += m.downloaded;
            }
        });

        Sample s;
        s.present = total > 0;

        if (dt > 0 && rxDelta / dt > RX_FLOOR)
            lastNetMoved_ = now;

        // wide enough to ride out the gap between parts
        s.active = s.present && now - lastNetMoved_ < NET_GRACE_S;

        // each part has its own BytesToDownload
        if (s.present && total != part_)
        {
            part_ = total;
            estDone_ = std::max((double)done, carry >= 0 ? carry / 100.0 * (double)total : 0.0);
        }

        // capped: wire bytes include protocol overhead
        if (s.active)
            estDone_ = std::min((double)total, std::max((double)done, estDone_ + rxDelta));

        s.part = total;
        s.percent = total > 0 ? 100.0f * (float)(estDone_ / (double)total) : 0.0f;
        s.jobDone = !s.present && last.percent >= 99.0f;
        return s;
    }

    void forget()
    {
        part_ = 0;
        estDone_ = 0.0;
    }

private:
    static constexpr double NET_GRACE_S = 10.0;

    double estDone_ = 0.0;
    unsigned long long part_ = 0;   // the BytesToDownload estDone_ belongs to
    double lastNetMoved_ = -1e9;
};

// Turns samples into the bar. An install is several apps downloaded one
// after another (game, Steamworks Common Redistributables, Proton, shader
// pre-cache); each fills its own bar from 0. The percent survives brief
// inactivity so an effect switching back resumes, and a finish holds a few
// seconds so the effect can celebrate before the rule switches away.
class Tracker
{
public:
    const Downloads& update()
    {
        double now = mono();

        // the 0.5 s rule tick and the per-frame effect share one scan
        if (now - lastScan_ < 1.0)
            return cached_;

        double dt = now - lastScan_;
        lastScan_ = now;

        // sampled even while Steam's reading is used, so a fallback never
        // integrates a stale delta; rx < lastRx is a counter reset
        unsigned long long rx = netRxBytes();
        double rxDelta = (lastRx_ != 0 && rx > lastRx_) ? (double)(rx - lastRx_) : 0.0;
        lastRx_ = rx;

        Sample s;
        bool exact = steamSample(s, last_);

        if (exact)
            manifests_.forget();
        else
            s = manifests_.sample(now, dt, rxDelta, last_,
                                  wasExact_ && wasPresent_ ? last_.percent : -1.0f);

        wasExact_ = exact;
        wasPresent_ = s.present;

        if (s.present)
            last_ = s;

        if (s.active)
        {
            idleSince_ = -1e9;
            finishHandled_ = false;
            finishedUntil_ = -1e9;

            cached_.percent = s.percent;
            cached_.active = true;
            cached_.finished = false;
            return cached_;
        }

        if (idleSince_ < 0.0)
            idleSince_ = now;

        if (!finishHandled_ && !s.present && s.jobDone && last_.part != 0)
        {
            finishedUntil_ = now + FINISH_HOLD_S;
            finishHandled_ = true;
        }

        if (now < finishedUntil_)
        {
            cached_.active = true;
            cached_.finished = true;
            cached_.percent = 100.0f;
            return cached_;
        }

        if ((finishHandled_ && now >= finishedUntil_) ||
            (!s.present && now - idleSince_ > RESET_GRACE_S))
        {
            manifests_.forget();
            last_ = Sample{};
            finishHandled_ = false;
            finishedUntil_ = -1e9;
            cached_ = Downloads{};
            return cached_;
        }

        // paused or stalled: keep the percent
        cached_.active = false;
        cached_.finished = false;
        return cached_;
    }

private:
    static constexpr double FINISH_HOLD_S = 5.0;
    static constexpr double RESET_GRACE_S = 30.0;

    Downloads cached_;
    ManifestEstimate manifests_;
    Sample last_;                   // the last sample with a download present
    bool wasExact_ = false;
    bool wasPresent_ = false;
    unsigned long long lastRx_ = 0;
    double lastScan_ = -1e9;
    double idleSince_ = -1e9;
    bool finishHandled_ = false;
    double finishedUntil_ = -1e9;

    static double mono()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
    }
};

inline const Downloads& downloads()
{
    if (const Downloads* sim = fakeDownloads())
        return *sim;

    static Tracker tracker;
    return tracker.update();
}

// `led --steam-status`: what each source sees and why
inline void dumpStatus(FILE* out, int debugPort)
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

    forEachManifest(libs, [&](const char* file, const std::string& path) {
        manifests++;

        Manifest m;
        if (!readManifest(path, m, true))
        {
            fprintf(out, "  %s: cannot open\n", file);
            return;
        }

        bool counts = m.running();

        fprintf(out, "  %s \"%s\": StateFlags=%lu BytesToDownload=%llu "
                     "BytesDownloaded=%llu -> %s\n",
                file, m.name.c_str(), m.flags, m.toDownload, m.downloaded,
                counts ? "COUNTS" : "ignored");

        for (const auto& d : m.depots)
            fprintf(out, "      per-depot: %s\n", d.c_str());

        if (counts)
        {
            total += m.toDownload;
            done += m.downloaded;
            counted++;
            return;
        }

        if (m.toDownload == 0)
            fprintf(out, "      reason: BytesToDownload is 0\n");
        if (m.flags & 512)
            fprintf(out, "      reason: paused (StateFlags bit 512)\n");
        if (!(m.flags & (256 | 1024)))
            fprintf(out, "      reason: no running bit 256/1024 in StateFlags\n");
    });

    fprintf(out, "manifests scanned: %d, counted as active: %d\n",
            manifests, counted);

    if (total > 0)
        fprintf(out, "totals: %llu / %llu bytes -> %.1f%% (manifest baseline; "
                     "the live bar advances this with network RX between the "
                     "manifest's infrequent flushes)\n",
                done, total, 100.0 * done / total);
    else
        fprintf(out, "totals: nothing counted -> percent 0\n");

    unsigned long long rx0 = netRxBytes();
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    timespec nap{1, 0};
    nanosleep(&nap, nullptr);

    unsigned long long rx1 = netRxBytes();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double rate = (dt > 0 && rx1 > rx0) ? (rx1 - rx0) / dt : 0.0;
    bool netActive = rate > RX_FLOOR;

    fprintf(out, "network RX: %.0f KiB/s (floor %.0f KiB/s) -> %s\n",
            rate / 1024.0, RX_FLOOR / 1024.0,
            netActive ? "downloading" : "idle/verify");

    fprintf(out, "steam_dl %s\n",
            (counted > 0 && netActive)
                ? "would be active (manifest counts + network flowing)"
                : "inactive (needs a counted manifest AND network throughput; "
                  "a finished download being verified counts but sends nothing)");

    if (debugPort <= 0)
    {
        fprintf(out, "debug port: off (set %s in the config, and pass the config: "
                     "led <config> --steam-status)\n", CONFIG_KEY_DEBUG_PORT);
        return;
    }

    Live live;
    std::string raw, err;

    if (!Cef::probe(debugPort, live, raw, err))
    {
        fprintf(out, "debug port %d: %s -> the daemon uses the manifests above\n",
                debugPort, err.c_str());
        return;
    }

    fprintf(out, "debug port %d: hooked into Steam -> the daemon uses this, not the manifests\n",
            debugPort);
    fprintf(out, "  app %u, state %s%s: %.0f / %.0f bytes, %.1f%%, %.0f KiB/s, %d queued\n",
            live.appid, live.state.c_str(), live.paused ? " (paused)" : "",
            live.done, live.total, live.percent, live.rate / 1024.0, live.queued);
    fprintf(out, "steam_dl %s\n", live.running ? "would be active" : "inactive (nothing running)");

    if (!raw.empty())
        fprintf(out, "raw hook state:\n%s\n", raw.c_str());
}

} // namespace steam
