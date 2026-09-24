#pragma once

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "pmbus.hpp"
#include "smu.hpp"

// /sys/class/hwmon sensor discovery and reading, shared by the
// load effect and the temp rule conditions
namespace hwmon
{
inline float readTemp(const std::string& path);
inline bool readTempOk(const std::string& path, float& v);
struct Reading;
inline void listTempFiles(const char* dirPath, std::vector<Reading>& out);

// Two temperature sources that live outside /sys/class/hwmon take the same
// chip:label spec and ride the same resolved-path plumbing, told apart by a
// prefix on the path every reader below dispatches on:
//   file:/run/bc250/gpu_vrm_temp   a plain file holding one number —
//                                  millidegrees (the sysfs convention) or
//                                  degrees; 1000 and up is read as
//                                  millidegrees
//   pmbus:CPU VRM                  a rail of the BC-250's VRM controller,
//                                  read over I2C by pmbus.hpp
//   smu:VRAM hotspot               a GDDR6 chip temperature, read from the
//                                  SMU by smu.hpp (off unless the config
//                                  opts in; see smu::Reader)
// The "chip" of a file spec is the word file and its label the path; a pmbus
// spec's labels are pmbus::RAILS, an smu spec's smu::SOURCES.
inline const char* FILE_PREFIX = "file:";
inline const char* PMBUS_PREFIX = "pmbus:";
inline const char* SMU_PREFIX = "smu:";

inline bool hasPrefix(const std::string& s, const char* prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

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

inline bool statExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// a resolved sensor path is still there: the sysfs file, the file behind a
// file: path, the controller behind a pmbus: one
inline bool fileExists(const std::string& path)
{
    if (hasPrefix(path, FILE_PREFIX))
        return statExists(path.substr(strlen(FILE_PREFIX)));
    if (hasPrefix(path, PMBUS_PREFIX))
        return pmbus::Reader::get().present();
    if (hasPrefix(path, SMU_PREFIX))
        return smu::Reader::get().present();
    return statExists(path);
}

// locate a chip's temp input by label; an empty label means the chip's
// first input. No match → "" so the caller can try the next candidate
inline std::string findSensor(const std::string& chip, const std::string& label)
{
    if (chip == "file")
        return !label.empty() && statExists(label) ? FILE_PREFIX + label : "";

    if (chip == "pmbus")
        return pmbus::railOf(label) >= 0 && pmbus::Reader::get().present()
                   ? PMBUS_PREFIX + label
                   : "";

    if (chip == "smu")
        return smu::sourceOf(label) >= 0 && smu::Reader::get().present()
                   ? SMU_PREFIX + label
                   : "";

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

        for (int i = 1; i <= 32; i++) // nct6683 exposes up to 32 temperature channels
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

// one thing under /sys/class/hwmon a fan header could follow: a labelled
// temperature (spec "chip:label", °C) or a pwm output (spec "chip:pwmN",
// read 0..255 and reported 0..100 %)
struct Reading
{
    std::string chip;
    std::string label; // the temp's label, or "pwmN"
    bool pwm = false;
    float value = 0;
};

// every such reading, chips and entries in a stable order. Unlabelled
// temperatures are skipped (the config can't name them), as are readings no
// thermistor produces: at or below 0 °C, or above TEMP_MAX (the NCT6686D's
// unconnected inputs read 0). Fan tachometers, voltages and currents are not
// sources and are not listed.
// LED_HWMON_ROOT points enumerate() at a stand-in tree (tests on a machine
// with no sensors); the daemon's own lookups always read the real one
inline std::vector<Reading> enumerate()
{
    std::vector<Reading> out;
    const char* env = getenv("LED_HWMON_ROOT");
    std::string root = env && *env ? env : "/sys/class/hwmon";
    DIR* dir = opendir(root.c_str());
    if (!dir)
        return out;

    std::vector<std::string> dirs;
    while (dirent* e = readdir(dir))
        if (e->d_name[0] != '.')
            dirs.push_back(root + "/" + e->d_name);
    closedir(dir);
    std::sort(dirs.begin(), dirs.end());

    for (const auto& base : dirs)
    {
        std::string chip = readFileLine(base + "/name");
        if (chip.empty())
            continue;

        for (int i = 1; i <= 32; i++) // nct6683 exposes up to 32 temperature channels
        {
            std::string input = base + "/temp" + std::to_string(i);
            if (!fileExists(input + "_input"))
                continue;
            std::string label = readFileLine(input + "_label");
            if (label.empty())
                continue;
            float v;
            if (!readTempOk(input + "_input", v))
                continue;
            out.push_back({chip, label, false, v});
        }

        for (int i = 1; i <= 8; i++)
        {
            std::string file = base + "/pwm" + std::to_string(i);
            if (!fileExists(file))
                continue;
            std::ifstream f(file);
            int raw = -1;
            f >> raw;
            if (raw < 0 || raw > 255)
                continue;
            out.push_back({chip, "pwm" + std::to_string(i), true, raw * 100.0f / 255.0f});
        }
    }

    // the VRM controller's rails, once the bus has been found and a rail
    // reads (asking starts the poller, so a watched catalogue lists them on
    // its next refresh)
    for (int i = 0; i < pmbus::RAIL_COUNT; i++)
    {
        float v;
        if (readTempOk(std::string(PMBUS_PREFIX) + pmbus::RAILS[i].label, v))
            out.push_back({"pmbus", pmbus::RAILS[i].label, false, v});
    }

    // the GDDR6 chips, once the SMU is patched (only when the config opted in,
    // so the enumerate is empty and the poller stays dark otherwise)
    for (int i = 0; i < smu::SOURCE_COUNT; i++)
    {
        float v;
        if (readTempOk(std::string(SMU_PREFIX) + smu::SOURCES[i].label, v))
            out.push_back({"smu", smu::SOURCES[i].label, false, v});
    }

    // temperatures other telemetry publishes as files: BC250-Telemetry
    // (github.com/onlinermm/BC250-Telemetry) writes its PMBus and GDDR6
    // readings as millidegree files under /run/bc250 — cpu_vrm_temp,
    // gpu_vrm_temp, memory_hotspot_temp, memory_avg_temp. Each that reads is
    // a "file" entry keyed by its path, so the phone can pick it rather than
    // type it; the directory is simply absent without that service.
    listTempFiles("/run/bc250", out);

    return out;
}

// every *_temp file in a directory that holds a usable temperature, as
// "file" entries labelled by path
inline void listTempFiles(const char* dirPath, std::vector<Reading>& out)
{
    DIR* dir = opendir(dirPath);
    if (!dir)
        return;
    std::vector<std::string> names;
    while (dirent* e = readdir(dir))
    {
        std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, "_temp") == 0)
            names.push_back(n);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (auto& n : names)
    {
        std::string path = std::string(dirPath) + "/" + n;
        float v;
        if (readTempOk(FILE_PREFIX + path, v))
            out.push_back({"file", path, false, v});
    }
}

inline float readTemp(const std::string& path)
{
    if (path.empty())
        return 0;

    if (hasPrefix(path, FILE_PREFIX) || hasPrefix(path, PMBUS_PREFIX) ||
        hasPrefix(path, SMU_PREFIX))
    {
        float v;
        return readTempOk(path, v) ? v : 0;
    }

    std::ifstream f(path);
    long millideg = 0;
    f >> millideg;

    return millideg / 1000.0f;
}

// a temperature a fan may act on: false when the file is missing or
// unreadable, and for a value no thermistor produces — at or below 0 °C
// (an unplugged input, which the NCT6686D reports as 0) or above TEMP_MAX. A
// fan curve fed 0 would run to its floor with the part it cools unwatched,
// so callers treat false as "no reading" and fall back.
// TEMP_MAX only has to catch garbage, never a real reading: a loaded VRM
// runs past 120 °C, and a hot part read as "no reading" drops its fan to
// the fallback exactly when it needs the curve's top. Nothing survives
// 200 °C, so a value above it is a misread.
inline const float TEMP_MAX = 200;

inline bool readTempOk(const std::string& path, float& v)
{
    if (path.empty())
        return false;

    if (hasPrefix(path, PMBUS_PREFIX))
    {
        int rail = pmbus::railOf(path.substr(strlen(PMBUS_PREFIX)));
        if (!pmbus::Reader::get().temp(rail, v))
            return false;
        return v > 0 && v <= TEMP_MAX;
    }

    if (hasPrefix(path, SMU_PREFIX))
    {
        int source = smu::sourceOf(path.substr(strlen(SMU_PREFIX)));
        if (!smu::Reader::get().temp(source, v))
            return false;
        return v > 0 && v <= TEMP_MAX;
    }

    if (hasPrefix(path, FILE_PREFIX))
    {
        // one number, millidegrees or degrees: no thermistor reads 1000 °C
        // and a millidegree value under 1000 is under 1 °C, rejected either way
        std::ifstream f(path.substr(strlen(FILE_PREFIX)));
        double d = 0;
        if (!(f >> d))
            return false;
        if (d >= 1000 || d <= -1000)
            d /= 1000;
        v = (float)d;
        return v > 0 && v <= TEMP_MAX;
    }

    std::ifstream f(path);
    long millideg = 0;
    if (!(f >> millideg))
        return false;
    v = millideg / 1000.0f;
    return v > 0 && v <= TEMP_MAX;
}

} // namespace hwmon
