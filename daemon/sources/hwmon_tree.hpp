#pragma once

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// The /sys/class/hwmon tree itself: where it is and which chips it holds.
// Shared by hwmon.hpp and the two BC-250 readers below it (pmbus.hpp,
// smu.hpp), which read through a kernel driver's chip when one is loaded
// instead of talking to the hardware themselves.
namespace hwmon
{
// the hwmon tree: /sys/class/hwmon, or LED_HWMON_ROOT's stand-in (tests on a
// machine with no sensors — every lookup below, and the fans' host outputs,
// then read and write the stand-in instead)
inline std::string root()
{
    const char* env = getenv("LED_HWMON_ROOT");
    return env && *env ? env : "/sys/class/hwmon";
}

inline std::string readFileLine(const std::string& path)
{
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return line;
}

inline bool statExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// the hwmon chips, as { dir, name }, in sorted directory order and one per
// name: a second chip of the same name can't be named apart, so the first
// is the one every lookup by name means (findSensor, findChipFile, and the
// phone's pickers through enumerate — all must agree on which chip that is)
struct Chip
{
    std::string dir;
    std::string name;
};
inline std::vector<Chip> chips()
{
    std::vector<Chip> out;
    const std::string top = root();
    DIR* dir = opendir(top.c_str());
    if (!dir)
        return out;
    std::vector<std::string> dirs;
    while (dirent* e = readdir(dir))
        if (e->d_name[0] != '.')
            dirs.push_back(top + "/" + e->d_name);
    closedir(dir);
    std::sort(dirs.begin(), dirs.end());

    for (auto& d : dirs)
    {
        std::string name = readFileLine(d + "/name");
        if (name.empty())
            continue;
        bool seen = false;
        for (auto& c : out)
            seen |= c.name == name;
        if (!seen)
            out.push_back({d, name});
    }
    return out;
}

// the directory of the chip of that name, "" when none is registered
inline std::string chipDir(const std::string& name)
{
    for (auto& c : chips())
        if (c.name == name)
            return c.dir;
    return "";
}

// <dir>/<type>N_input whose <type>N_label reads label, "" when none does.
// hwmon numbers voltages from 0 and everything else from 1; both are tried.
inline std::string labelledInput(const std::string& dir, const char* type, const std::string& label)
{
    for (int i = 0; i <= 32; i++)
    {
        std::string base = dir + "/" + type + std::to_string(i);
        if (readFileLine(base + "_label") == label)
            return base + "_input";
    }
    return "";
}

// one sysfs value in the file's own unit (milli-whatever), false when the
// file is gone or the driver's read failed
inline bool readLong(const std::string& path, long& v)
{
    std::ifstream f(path);
    return path.size() && (f >> v);
}

} // namespace hwmon
