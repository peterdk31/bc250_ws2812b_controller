#include "condition.hpp"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <vector>
#include "hwmon.hpp"

class AlwaysCondition : public Condition
{
public:
    bool eval() override { return true; }
};

class TempCondition : public Condition
{
public:
    TempCondition(const Config& cfg, bool above, float threshold)
        : above(above), threshold(threshold)
    {
        spec = cfg.get("sensors", hwmon::DEFAULT_SENSORS);
        path = hwmon::findSensorFromSpec(spec);

        if (path.empty())
            fprintf(stderr, "temp condition: no sensor found yet, "
                            "will keep looking\n");
    }

    bool eval() override
    {
        // the sensor module may load after we start; keep retrying so
        // temp rules (incl. the overheat alarm) come alive when it does
        if (path.empty())
        {
            path = hwmon::findSensorFromSpec(spec);

            if (path.empty())
                return false;

            fprintf(stderr, "temp condition: using %s\n", path.c_str());
        }

        float t = hwmon::readTemp(path);

        return above ? t > threshold : t < threshold;
    }

private:
    std::string spec;
    std::string path;
    bool above;
    float threshold;
};

class GpuLoadCondition : public Condition
{
public:
    GpuLoadCondition(bool above, float threshold)
        : above(above), threshold(threshold)
    {
        if (!gpuLoad.available())
            fprintf(stderr, "gpu_load condition: no gpu load source "
                            "found yet, will keep looking\n");
    }

    bool eval() override
    {
        // amdgpu may load after we start; available() keeps retrying
        // discovery, and a strip with no GPU shouldn't satisfy
        // `gpu_load<N` just because the reading defaults to 0
        if (!gpuLoad.available())
            return false;

        float load = gpuLoad.readPercent();

        return above ? load > threshold : load < threshold;
    }

private:
    hwmon::GpuLoad gpuLoad;
    bool above;
    float threshold;
};

// busy percent of /proc/stat ticks between evals (the daemon's 0.5 s
// rule tick); reports 0 until the second eval
class CpuLoadCondition : public Condition
{
public:
    CpuLoadCondition(bool above, float threshold)
        : above(above), threshold(threshold)
    {
        hwmon::readCpuCounters(prevBusy, prevTotal);
    }

    bool eval() override
    {
        unsigned long long busy, total;

        if (hwmon::readCpuCounters(busy, total) && total > prevTotal)
        {
            load = 100.0f * (busy - prevBusy) / (total - prevTotal);
            prevBusy = busy;
            prevTotal = total;
        }

        return above ? load > threshold : load < threshold;
    }

private:
    unsigned long long prevBusy = 0;
    unsigned long long prevTotal = 0;
    float load = 0;
    bool above;
    float threshold;
};

class ProcCondition : public Condition
{
public:
    ProcCondition(std::string name) : name(std::move(name)) {}

    bool eval() override
    {
        DIR* dir = opendir("/proc");
        if (!dir) return false;

        bool found = false;

        while (dirent* e = readdir(dir))
        {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;

            // note: comm is truncated to 15 chars by the kernel
            std::string comm = hwmon::readFileLine(
                std::string("/proc/") + e->d_name + "/comm");

            if (comm == name)
            {
                found = true;
                break;
            }
        }

        closedir(dir);
        return found;
    }

private:
    std::string name;
};

class FileCondition : public Condition
{
public:
    FileCondition(std::string path) : path(std::move(path)) {}

    bool eval() override
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

private:
    std::string path;
};

class NotCondition : public Condition
{
public:
    NotCondition(std::unique_ptr<Condition> inner) : inner(std::move(inner)) {}

    bool eval() override { return !inner->eval(); }

private:
    std::unique_ptr<Condition> inner;
};

class AndCondition : public Condition
{
public:
    AndCondition(std::vector<std::unique_ptr<Condition>> terms)
        : terms(std::move(terms)) {}

    bool eval() override
    {
        for (auto& t : terms)
            if (!t->eval())
                return false;

        return true;
    }

private:
    std::vector<std::unique_ptr<Condition>> terms;
};

class OrCondition : public Condition
{
public:
    OrCondition(std::vector<std::unique_ptr<Condition>> terms)
        : terms(std::move(terms)) {}

    bool eval() override
    {
        for (auto& t : terms)
            if (t->eval())
                return true;

        return false;
    }

private:
    std::vector<std::unique_ptr<Condition>> terms;
};

static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";

    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static std::unique_ptr<Condition> parseAtom(const std::string& spec,
                                            const Config& cfg)
{
    if (spec == "always")
        return std::make_unique<AlwaysCondition>();

    if (spec.rfind("proc:", 0) == 0)
        return std::make_unique<ProcCondition>(spec.substr(5));

    if (spec.rfind("file:", 0) == 0)
        return std::make_unique<FileCondition>(spec.substr(5));

    if (spec.rfind("temp", 0) == 0 && spec.size() > 5 &&
        (spec[4] == '>' || spec[4] == '<'))
    {
        return std::make_unique<TempCondition>(
            cfg, spec[4] == '>', (float)atof(spec.c_str() + 5));
    }

    if (spec.rfind("gpu_load", 0) == 0)
    {
        if (spec.size() == 8)
            return std::make_unique<GpuLoadCondition>(true, 20.0f);

        if (spec.size() > 9 && (spec[8] == '>' || spec[8] == '<'))
            return std::make_unique<GpuLoadCondition>(
                spec[8] == '>', (float)atof(spec.c_str() + 9));
    }

    if (spec.rfind("cpu_load", 0) == 0)
    {
        if (spec.size() == 8)
            return std::make_unique<CpuLoadCondition>(true, 20.0f);

        if (spec.size() > 9 && (spec[8] == '>' || spec[8] == '<'))
            return std::make_unique<CpuLoadCondition>(
                spec[8] == '>', (float)atof(spec.c_str() + 9));
    }

    return nullptr;
}

// an atom with any number of leading '!'
static std::unique_ptr<Condition> parseTerm(std::string spec,
                                            const Config& cfg)
{
    bool negate = false;

    while (!spec.empty() && spec[0] == '!')
    {
        negate = !negate;
        spec = trim(spec.substr(1));
    }

    auto cond = parseAtom(spec, cfg);

    if (cond && negate)
        return std::make_unique<NotCondition>(std::move(cond));

    return cond;
}

// terms separated by '&', all must hold
static std::unique_ptr<Condition> parseAll(const std::string& spec,
                                           const Config& cfg)
{
    std::vector<std::unique_ptr<Condition>> terms;
    size_t start = 0;

    while (true)
    {
        size_t amp = spec.find('&', start);

        auto term = parseTerm(trim(spec.substr(start, amp == std::string::npos
                                                ? std::string::npos
                                                : amp - start)), cfg);

        if (!term)
            return nullptr;

        terms.push_back(std::move(term));

        if (amp == std::string::npos)
            break;

        start = amp + 1;
    }

    if (terms.size() == 1)
        return std::move(terms[0]);

    return std::make_unique<AndCondition>(std::move(terms));
}

// '|' alternatives of '&' groups: & binds tighter than |
std::unique_ptr<Condition> parseCondition(const std::string& spec,
                                          const Config& cfg)
{
    std::vector<std::unique_ptr<Condition>> alts;
    size_t start = 0;

    while (true)
    {
        size_t bar = spec.find('|', start);

        auto alt = parseAll(spec.substr(start, bar == std::string::npos
                                         ? std::string::npos
                                         : bar - start), cfg);

        if (!alt)
            return nullptr;

        alts.push_back(std::move(alt));

        if (bar == std::string::npos)
            break;

        start = bar + 1;
    }

    if (alts.size() == 1)
        return std::move(alts[0]);

    return std::make_unique<OrCondition>(std::move(alts));
}
