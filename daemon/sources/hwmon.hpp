#pragma once

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fstream>
#include <string>
#include <vector>

// /sys/class/hwmon sensor discovery and reading, shared by the
// load effect and the temp rule conditions
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

// amdgpu exposes GPU load two ways: gpu_busy_percent (plain text,
// backed by the GPU_LOAD sensor) and gpu_metrics (the binary SMU
// table). The BC-250's cyan skillfish doesn't implement the GPU_LOAD
// sensor, so gpu_busy_percent never appears and only gpu_metrics
// carries activity — the same file MangoHud reads, which is why
// MangoHud shows load where the sysfs file can't.

// first /sys/class/drm/card* that has the given device file
inline std::string findCardFile(const char* file)
{
    DIR* dir = opendir("/sys/class/drm");
    if (!dir) return "";

    std::string found;

    while (dirent* e = readdir(dir))
    {
        if (strncmp(e->d_name, "card", 4) != 0)
            continue;

        std::string path = std::string("/sys/class/drm/") + e->d_name
            + "/device/" + file;

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

// average_gfx_activity out of a gpu_metrics table, in percent, or -1
// when the file is unreadable, the layout is unknown, or the driver
// left the field unpopulated (the table is prefilled with 0xff).
// Offsets follow the kernel's kgd_pp_interface.h: format 1 is
// desktop GPUs, 2 is APUs, 3 is newer APUs; the *_0 content revisions
// carry system_clock_counter ahead of the data, later ones moved it
inline float readGpuMetricsActivity(const std::string& path, bool& centi)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return -1;

    uint8_t buf[64];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    if (n < 4)
        return -1;

    uint8_t format = buf[2];
    uint8_t content = buf[3];

    size_t off;

    if (format == 1)      off = content == 0 ? 24 : 16;
    else if (format == 2) off = content == 0 ? 36 : 28;
    else if (format == 3) off = 42;
    else return -1;

    if (n < off + 2)
        return -1;

    uint16_t v = buf[off] | (buf[off + 1] << 8);

    if (v == 0xffff)
        return -1;

    // some SMU firmwares report centipercent; once a value can only
    // be centipercent, divide everything after (MangoHud's quirk —
    // stateless division would misread idle centipercent as percent)
    if (v > 100)
        centi = true;

    if (centi)
        v /= 100;

    return v > 100 ? 100 : (float)v;
}

// GPU load in percent from gpu_metrics, falling back to
// gpu_busy_percent; amdgpu may load after we start, so discovery is
// retried on later reads until a source turns up
class GpuLoad
{
public:
    // 0-100, or 0 while no source is available
    float readPercent()
    {
        if (metricsPath.empty() && busyPath.empty() && !discover())
            return 0;

        if (!metricsPath.empty())
        {
            float v = readGpuMetricsActivity(metricsPath, centi);

            if (v >= 0)
                return v;
        }

        if (!busyPath.empty())
        {
            float v = (float)atof(readFileLine(busyPath).c_str());
            return v < 0 ? 0 : v > 100 ? 100 : v;
        }

        return 0;
    }

    bool available() { return discover(); }

private:
    bool discover()
    {
        if (!metricsPath.empty() || !busyPath.empty())
            return true;

        // a gpu_metrics file only counts if activity actually parses
        // out of it: mainline cyan skillfish publishes the table with
        // the activity field unfilled, and busy_percent should win
        // over a half-working table elsewhere
        std::string metrics = findCardFile("gpu_metrics");

        if (!metrics.empty()
            && readGpuMetricsActivity(metrics, centi) >= 0)
        {
            metricsPath = metrics;
            fprintf(stderr, "gpu load: using %s\n", metrics.c_str());
            return true;
        }

        busyPath = findCardFile("gpu_busy_percent");

        if (!busyPath.empty())
        {
            fprintf(stderr, "gpu load: using %s\n", busyPath.c_str());
            return true;
        }

        return false;
    }

    std::string metricsPath;
    std::string busyPath;
    bool centi = false;
};

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
