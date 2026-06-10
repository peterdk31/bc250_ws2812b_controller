#pragma once

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fstream>
#include <string>
#include <vector>

// /sys/class/hwmon sensor discovery and reading, shared by the
// cpu_temp effect and the temp rule conditions
namespace hwmon
{

// Tctl first when k10temp is present; the BC-250's NCT6686D registers
// as "nct6686" under both the nct6687d driver (label "CPU") and the
// in-kernel nct6683 driver (label "AMD TSI Addr 98h"). A bare chip
// name (no label) falls back to that chip's temp1_input.
inline const char* DEFAULT_SENSORS =
    "k10temp:Tctl,"
    "nct6686:CPU,nct6687:CPU,"
    "nct6686:AMD TSI Addr 98h,nct6683:AMD TSI Addr 98h,"
    "nct6686";

inline std::string readFileLine(const std::string& path)
{
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    return line;
}

inline std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> parts;
    size_t start = 0;

    while (start <= s.size())
    {
        size_t end = s.find(sep, start);
        if (end == std::string::npos) end = s.size();

        if (end > start)
            parts.push_back(s.substr(start, end - start));

        start = end + 1;
    }

    return parts;
}

inline bool fileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// locate a chip's temp input by label; an empty label means the chip's
// first input. No match → "" so the caller can try the next candidate
inline std::string findSensor(const std::string& chip, const std::string& label)
{
    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir) return "";

    std::string found;

    while (dirent* e = readdir(dir))
    {
        if (e->d_name[0] == '.')
            continue;

        std::string base = std::string("/sys/class/hwmon/") + e->d_name;

        if (readFileLine(base + "/name") != chip)
            continue;

        if (label.empty())
        {
            if (fileExists(base + "/temp1_input"))
                found = base + "/temp1_input";
            break;
        }

        for (int i = 1; i <= 20; i++)
        {
            std::string input = base + "/temp" + std::to_string(i);

            if (readFileLine(input + "_label") == label
                && fileExists(input + "_input"))
            {
                found = input + "_input";
                break;
            }
        }

        break;
    }

    closedir(dir);
    return found;
}

// "k10temp:Tctl,nct6687:CPU" → path of the first candidate present
inline std::string findSensorFromSpec(const std::string& spec)
{
    for (const auto& candidate : split(spec, ','))
    {
        size_t colon = candidate.find(':');

        std::string chip = candidate.substr(0, colon);
        std::string label =
            colon == std::string::npos ? "" : candidate.substr(colon + 1);

        std::string path = findSensor(chip, label);

        if (!path.empty())
            return path;
    }

    return "";
}

// aggregate "cpu" line of /proc/stat; busy excludes idle and iowait.
// load over an interval is delta busy / delta total between two reads
inline bool readCpuCounters(unsigned long long& busy,
                            unsigned long long& total)
{
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return false;

    unsigned long long user = 0, nice = 0, sys = 0, idle = 0,
                       iowait = 0, irq = 0, softirq = 0, steal = 0;

    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle, &iowait, &irq, &softirq,
                   &steal);
    fclose(f);

    if (n < 4)
        return false;

    total = user + nice + sys + idle + iowait + irq + softirq + steal;
    busy = total - idle - iowait;
    return true;
}

// amdgpu reports 0-100 in /sys/class/drm/card*/device/gpu_busy_percent;
// first card that has it wins
inline std::string findGpuLoadPath()
{
    DIR* dir = opendir("/sys/class/drm");
    if (!dir) return "";

    std::string found;

    while (dirent* e = readdir(dir))
    {
        if (strncmp(e->d_name, "card", 4) != 0)
            continue;

        std::string path = std::string("/sys/class/drm/") + e->d_name
            + "/device/gpu_busy_percent";

        struct stat st;

        if (stat(path.c_str(), &st) == 0)
        {
            found = path;
            break;
        }
    }

    closedir(dir);
    return found;
}

inline void listChips()
{
    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir) return;

    while (dirent* e = readdir(dir))
    {
        if (e->d_name[0] == '.')
            continue;

        std::string base = std::string("/sys/class/hwmon/") + e->d_name;

        fprintf(stderr, "  %s (%s)\n",
                readFileLine(base + "/name").c_str(), base.c_str());
    }

    closedir(dir);
}

inline float readTemp(const std::string& path)
{
    if (path.empty())
        return 0;

    std::ifstream f(path);
    long millideg = 0;
    f >> millideg;

    return millideg / 1000.0f;
}

} // namespace hwmon
