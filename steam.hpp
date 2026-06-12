#pragma once

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>

// Steam download tracking by polling steamapps/appmanifest_*.acf:
// Steam rewrites the manifests as chunks land, so their byte counters
// are the standard way to watch downloads from outside. Shared by the
// steam_dl condition and the progress effect
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

    while (fgets(line, sizeof line, f))
    {
        if (!vdfPair(line, key, val))
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

// state of all Steam downloads, rescanned at most every 2 s so the
// 0.5 s rule tick and the per-frame effect share one scan's cost.
// When the byte counts stop moving for 2 minutes the download is
// really paused or stalled (Steam doesn't always set the paused bit)
// and stops counting as active until they move again
inline const Downloads& downloads()
{
    static Downloads cached;
    static double lastScan = -1e9;
    static double lastMoved = 0;
    static unsigned long long lastDone = ~0ull;

    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;

    if (now - lastScan < 2.0)
        return cached;

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

    if (total == 0)
    {
        cached = Downloads{};
        lastDone = ~0ull;
        return cached;
    }

    if (done != lastDone)
    {
        lastDone = done;
        lastMoved = now;
    }

    cached.percent = 100.0f * done / total;
    cached.active = now - lastMoved < 120.0;
    return cached;
}

} // namespace steam
