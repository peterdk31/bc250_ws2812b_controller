#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include "steam.hpp"

// The installed games, from the appmanifests (steam.hpp) — so the list is
// there with Steam closed, and the phone can keep it for while the host is
// off. Steam's own tools (Proton, the runtimes, the redistributables) have
// manifests too and are left out.
//
// Newer games keep their store art under a hashed folder on Steam's CDN
// (store_item_assets/steam/apps/<appid>/<hash>/header.jpg), which the appid
// alone can't name. Steam's library cache on disk mirrors that folder, so the
// path is read from there; some games have only library_header.jpg.
namespace steam
{

struct Game
{
    unsigned appid = 0;
    std::string name;
    long lastPlayed = 0;    // unix seconds, 0 = never
    std::string art;        // "<hash>/<file>" under the appid, "" = not hashed
};

inline bool isTool(const std::string& name)
{
    static const char* const PREFIXES[] = {
        "Proton", "Steam Linux Runtime", "Steamworks Common Redistributables", "SteamVR",
    };
    for (const char* p : PREFIXES)
        if (name.rfind(p, 0) == 0)
            return true;
    return false;
}

inline std::string artPath(const std::vector<std::string>& libs, unsigned appid)
{
    static const char* const FILES[] = {"header.jpg", "library_header.jpg"};
    struct stat st;

    for (const auto& lib : libs)
    {
        std::string dir = lib + "/../appcache/librarycache/" + std::to_string(appid);
        if (stat((dir + "/header.jpg").c_str(), &st) == 0)
            return "";

        std::vector<std::string> hashes;
        if (DIR* d = opendir(dir.c_str()))
        {
            while (dirent* e = readdir(d))
                if (e->d_name[0] != '.')
                    hashes.push_back(e->d_name);
            closedir(d);
        }
        for (const char* file : FILES)
            for (const auto& h : hashes)
                if (stat((dir + "/" + h + "/" + file).c_str(), &st) == 0)
                    return h + "/" + file;
    }
    return "";
}

// most recently played first, never-played ones by name after them
inline std::vector<Game> installedGames()
{
    std::vector<Game> games;
    const std::vector<std::string> libs = libraries();

    forEachManifest(libs, [&](const char*, const std::string& path) {
        Manifest m;
        if (!readManifest(path, m) || !m.appid || m.name.empty() || isTool(m.name))
            return;
        for (const Game& g : games)
            if (g.appid == m.appid)
                return; // the same app in two libraries
        games.push_back({m.appid, m.name, m.lastPlayed, artPath(libs, m.appid)});
    });

    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        if (a.lastPlayed != b.lastPlayed)
            return a.lastPlayed > b.lastPlayed;
        return a.name < b.name;
    });
    return games;
}

} // namespace steam
