#pragma once

#include <string>
#include "progress.hpp"
#include "steam.hpp"
#include "steam_launch.hpp"

// Every bar the progress effect can draw, by the name a rule uses for both:
// the condition `steam_dl` is true while that source is active, and the
// effect's "source": "steam_dl" draws it. A new source is a row here.
namespace progress
{

struct Source
{
    const char* name;
    const Reading& (*read)();
};

inline const Source SOURCES[] = {
    {"steam_dl", steam::downloads},     // a Steam download, part by part
    {"steam_launch", steam::launch},    // a game launch's counted steps (shaders)
};

// null for a name that isn't one
inline const Source* find(const std::string& name)
{
    for (const Source& s : SOURCES)
        if (name == s.name)
            return &s;
    return nullptr;
}

inline std::string names()
{
    std::string out;
    for (const Source& s : SOURCES)
        out += (out.empty() ? "" : ", ") + std::string(s.name);
    return out;
}

} // namespace progress
