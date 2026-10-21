#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include "steam.hpp"

// The installed games, from the appmanifests (steam.hpp) — so the list is
// there with Steam closed, and the phone can keep it for while the host is
// off. Steam's own tools (Proton, the runtimes, the redistributables) have
// manifests too and are left out.
namespace steam
{

struct Game
{
    unsigned appid = 0;
    std::string name;
    long lastPlayed = 0;    // unix seconds, 0 = never
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

// most recently played first, never-played ones by name after them
inline std::vector<Game> installedGames()
{
    std::vector<Game> games;

    forEachManifest(libraries(), [&](const char*, const std::string& path) {
        Manifest m;
        if (!readManifest(path, m) || !m.appid || m.name.empty() || isTool(m.name))
            return;
        for (const Game& g : games)
            if (g.appid == m.appid)
                return; // the same app in two libraries
        games.push_back({m.appid, m.name, m.lastPlayed});
    });

    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        if (a.lastPlayed != b.lastPlayed)
            return a.lastPlayed > b.lastPlayed;
        return a.name < b.name;
    });
    return games;
}

} // namespace steam
