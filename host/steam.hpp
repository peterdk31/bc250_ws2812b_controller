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
    bool active = false;   // a download is running and bytes are moving
    float percent = 0.0f;  // 0..100 across all active downloads
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

// state of all Steam downloads, rescanned at most every 1 s so the
// 0.5 s rule tick and the per-frame effect share one scan's cost.
//
// percent comes from the manifests' top-level byte totals, but "is it
// downloading right now" comes from the in-flight chunk bytes under
// steamapps/downloading/: Steam rewrites the manifests only every minute
// or so -- far too coarse to tell a deliberate pause from a slow patch --
// while the download cache grows continuously during a transfer and
// freezes the instant it's paused. So active tracks recent movement of
// that cache. A manifest with no download in a running state is held
// briefly (the empty-scan grace) so a transient mid-rewrite scan doesn't
// blank the bar.
inline const Downloads& downloads()
{
    static Downloads cached;
    static double lastScan = -1e9;
    static double lastCacheMoved = -1e9;
    static unsigned long long lastCache = 0;
    static unsigned long long peakDone = 0;

    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;

    if (now - lastScan < 1.0)
        return cached;

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

    // the download cache is the activity authority: it grows while
    // downloading and freezes on pause. active just asks whether it
    // moved within the window. With a 1 s scan a 2 s window tolerates a
    // single dead tick (a brief network blip) yet clears ~2 s after a
    // real pause -- about the unpause latency. No manifest-based grace
    // anymore: the cache, not the manifest's flicker-prone StateFlags,
    // decides activity, so a stale empty manifest scan can't blank the
    // bar. Drop the window toward 1 s for near-instant pause, at the
    // cost of a flicker on any one-second stall.
    bool moving = now - lastCacheMoved < 2.0;

    if (!moving)
    {
        // nothing landing -> paused / finished / gone: inactive, no bar
        cached = Downloads{};
        peakDone = 0;
        return cached;
    }

    if (total > 0)
    {
        // the manifest's BytesDownloaded is accurate but Steam rewrites
        // it only every minute or so, so a bar driven off it sits frozen
        // between flushes and only jumps when Steam flushes (e.g. on a
        // pause). The download cache grows in real time, so take the live
        // figure as the larger of the two over the (stable) total, and
        // hold a high-water mark so a cache flush-to-disk can't make the
        // bar read low or run backward. A momentary empty manifest scan
        // (total == 0 mid-rewrite) just keeps the last percent.
        unsigned long long live = cache > done ? cache : done;

        if (live > peakDone) peakDone = live;
        if (peakDone > total) peakDone = total;

        cached.percent = 100.0f * peakDone / total;
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
        fprintf(out, "totals: %llu / %llu bytes -> %.1f%% (steam_dl active)\n",
                done, total, 100.0 * done / total);
    else
        fprintf(out, "totals: nothing counted -> percent 0, steam_dl inactive "
                     "(this is why the bar is blank)\n");
}

} // namespace steam
