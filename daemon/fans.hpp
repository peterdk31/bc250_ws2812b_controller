#pragma once

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <utility>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config_loader.hpp"
#include "hwmon.hpp"
#include "protocol.hpp"
#include "sink.hpp"

// The daemon's half of the fan controller (README "Fans"). The receiver
// drives the PWM headers; this side decides what they should run, from the
// config's "fans" block:
//
//     "fans": {
//         "hysteresis": 3, "ramp": 5, "boost_seconds": 5,
//         "header1": { "enabled": true, "name": "pump", "source": "constant",
//                      "curve": "65", "boost": 100, "fallback": 100 },
//         "header2": { ... "source": "temp", "curve": "45:35 60:55 75:100",
//                      "boost": null, "fallback": 100 },
//         ...
//     }
//
// Every header has the same six keys. `source` is what the curve reads:
// `constant` (no reading; the curve is one value), `temp` (the top-level
// `sensors` pick, °C), a hwmon `chip:label` temperature (same syntax as the
// sensors list, °C), a hwmon `chip:pwmN` output (the board's own fan header,
// read as 0..100 %), or `cpu_load` / `gpu_load` (0..100 %). `curve` is
// "x:percent" points, linear between and flat beyond the ends; the x unit is
// the source's. `boost` is the duty for the first boost_seconds after the host
// powers on (null = sits it out) and `fallback` what the receiver runs
// whenever this daemon isn't driving the header. Both of those, plus
// boost_seconds, are the receiver's standalone settings: pushed once at
// startup (CMD_FAN_STANDALONE) and also baked into its fancfg partition at
// flash time from this same block (tools/fancfg.py). A disabled header is
// simply not the daemon's — nothing is ever sent for it.
//
// The curves run on the rules' 0.5 s tick, sharing the reads the rule
// conditions already do. Their output goes out as CMD_FAN_LIVE whole percents
// when something changed, plus a refresh every few seconds so the receiver
// can treat a silence as "the daemon is gone" and fall back — which is also
// why a live duty is never persisted on the receiver.
namespace fans
{
static const int CHANNELS = proto::FAN_CHANNELS;
static const double REFRESH_S = 5.0; // resend unchanged live duties this often
                                     // (the receiver drops them after 15 s of
                                     // silence — see firmware fan.cpp)

struct Point
{
    float x, y;
};

struct Header
{
    int slot = 0;                    // 0-based; header1 is slot 0
    std::string key;                 // "header1"
    bool enabled = false;
    std::string name;
    std::string source;              // as written

    enum Kind { Constant, Temp, Pwm, CpuLoad, GpuLoad } kind = Constant;
    std::vector<Point> curve;        // sorted by x; Constant = one point
    int boost = -1;                  // percent, -1 = null (no boost)
    int fallback = 100;

    // runtime
    std::string spec;                // hwmon candidates (Temp) / chip (Pwm)
    std::string pwmFile;             // "pwm1" (Pwm)
    std::string path;                // resolved sysfs file, "" = not yet
    bool reported = false;           // "no sensor yet" said once
    bool haveIn = false;
    float effIn = 0;                 // hysteresis-filtered input
    bool haveOut = false;
    float out = 0;                   // ramped output
    float lastIn = 0;                // last raw reading (for --fan-status)
    bool lastInOk = false;

    // the curve at x: linear between points, flat beyond the ends
    float eval(float x) const
    {
        if (curve.empty())
            return fallback;
        if (x <= curve.front().x)
            return curve.front().y;
        if (x >= curve.back().x)
            return curve.back().y;

        for (size_t i = 1; i < curve.size(); i++)
        {
            if (x <= curve[i].x)
            {
                const Point& a = curve[i - 1];
                const Point& b = curve[i];
                float t = (x - a.x) / (b.x - a.x);
                return a.y + t * (b.y - a.y);
            }
        }

        return curve.back().y;
    }
};

// find /sys/class/hwmon/<chip>/<file> (a pwmN output, say); "" when absent
inline std::string findChipFile(const std::string& chip, const std::string& file)
{
    DIR* dir = opendir("/sys/class/hwmon");
    if (!dir)
        return "";

    std::string found;

    while (dirent* e = readdir(dir))
    {
        if (e->d_name[0] == '.')
            continue;

        std::string base = std::string("/sys/class/hwmon/") + e->d_name;

        if (hwmon::readFileLine(base + "/name") != chip)
            continue;

        if (hwmon::fileExists(base + "/" + file))
            found = base + "/" + file;

        break;
    }

    closedir(dir);
    return found;
}

class Controller
{
public:
    // read and validate the "fans" block. Returns false (having said what is
    // wrong, "fans.header2.curve: ...") on a bad block, so the daemon can
    // refuse to start the way it does for a bad rule; true with no block or no
    // enabled header, in which case active() is false and nothing is ever
    // sent.
    bool load(const Config& cfg)
    {
        const json::Value* block = cfg.root().find("fans");

        if (!block)
            return true;

        if (!block->isObject())
        {
            fprintf(stderr, "fans: expected an object\n");
            return false;
        }

        const std::string sensors = cfg.get("sensors", hwmon::DEFAULT_SENSORS);

        for (auto& m : block->members)
        {
            const std::string& k = m.first;
            const json::Value& v = m.second;

            if (k == "hysteresis")
            {
                if (!v.isNumber() || v.number < 0)
                    return bad("fans.hysteresis", "expected a number of °C, 0 or more");
                hysteresis_ = (float)v.number;
            }
            else if (k == "ramp")
            {
                if (!v.isNumber() || v.number < 0)
                    return bad("fans.ramp", "expected percent per second, 0 or more");
                ramp_ = (float)v.number;
            }
            else if (k == "boost_seconds")
            {
                if (!v.isNumber() || v.number < 0 || v.number > 255)
                    return bad("fans.boost_seconds", "expected seconds, 0..255");
                boostSeconds_ = (int)v.number;
            }
            else if (k == "pins")
            {
                // the flasher's business (tools/fancfg.py); only the shape is
                // checked here so a typo still fails before the fans run
                if (!v.isString() && !v.isArray())
                    return bad("fans.pins", "expected \"5,6,7,10\" (or an array)");
            }
            else if (k.rfind("header", 0) == 0)
            {
                int n = atoi(k.c_str() + 6);
                if (n < 1 || n > CHANNELS || k != "header" + std::to_string(n))
                    return bad("fans." + k, "no such header (header1..header" +
                                                std::to_string(CHANNELS) + ")");

                Header h;
                h.slot = n - 1;
                h.key = k;
                if (!loadHeader(v, sensors, h))
                    return false;

                for (auto& o : headers_)
                    if (o.slot == h.slot)
                        return bad("fans." + k, "given twice");

                headers_.push_back(std::move(h));
            }
            else
                return bad("fans." + k, "unknown key");
        }

        for (auto& h : headers_)
            enabled_ += h.enabled;

        return true;
    }

    bool active() const { return enabled_ > 0; }

    // the receiver's standalone settings — once, at startup
    void pushStandalone(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (!active())
            return;

        uint8_t p[1 + 2 * CHANNELS];
        memset(p, proto::FAN_NONE, sizeof p);
        p[0] = (uint8_t)boostSeconds_;

        for (auto& h : headers_)
        {
            if (!h.enabled)
                continue;
            p[1 + 2 * h.slot] = (uint8_t)h.fallback;
            p[2 + 2 * h.slot] = h.boost < 0 ? proto::FAN_NONE : (uint8_t)h.boost;
        }

        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_STANDALONE, p, sizeof p);
    }

    // one evaluation: read every enabled header's source, run the curves, and
    // send the live duties if they changed (or the refresh is due). Call it
    // on the rules tick; it costs nothing when nothing is enabled.
    void tick(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (!active())
            return;

        float dt = lastTick_ > 0 ? (float)(now - lastTick_) : 0.5f;
        lastTick_ = now;

        readShared();

        uint8_t p[CHANNELS];
        memset(p, proto::FAN_NONE, sizeof p);

        for (auto& h : headers_)
            if (h.enabled)
                p[h.slot] = (uint8_t)compute(h, dt);

        bool changed = memcmp(p, live_, sizeof p) != 0;

        if (changed || now - lastSent_ >= REFRESH_S)
        {
            memcpy(live_, p, sizeof p);
            lastSent_ = now;

            for (auto& s : sinks)
                s->sendCommand(proto::CMD_FAN_LIVE, p, sizeof p);
        }
    }

    // ./led <config> --fan-status: what each header resolves to and would run
    // right now (two ticks, so cpu_load has a delta to report)
    void dumpStatus(FILE* out)
    {
        if (headers_.empty())
        {
            fprintf(out, "fans: no \"fans\" block in the config\n");
            return;
        }

        std::vector<std::unique_ptr<Sink>> none;
        tick(0.0, none);
        struct timespec ts = {0, 500 * 1000 * 1000};
        nanosleep(&ts, nullptr);
        tick(0.5, none);

        fprintf(out, "fans: hysteresis %g °C, ramp %g %%/s, boost %d s\n",
                hysteresis_, ramp_, boostSeconds_);

        for (auto& h : headers_)
        {
            fprintf(out, "  %s %-10s %s", h.key.c_str(), h.name.c_str(),
                    h.enabled ? "enabled " : "disabled");

            if (h.kind == Header::Constant)
                fprintf(out, "  constant %g%%", h.curve[0].y);
            else
            {
                fprintf(out, "  %s", h.source.c_str());
                if (h.kind == Header::Temp || h.kind == Header::Pwm)
                    fprintf(out, " -> %s", h.path.empty() ? "(not found)" : h.path.c_str());
                if (h.lastInOk)
                    fprintf(out, " = %g%s", h.lastIn, h.kind == Header::Temp ? " °C" : " %");
                else
                    fprintf(out, " = (no reading)");
            }

            if (h.enabled)
                fprintf(out, "  -> %d%%", (int)live_[h.slot]);
            fprintf(out, "   boost %s, fallback %d%%\n",
                    h.boost < 0 ? "none" : (std::to_string(h.boost) + "%").c_str(),
                    h.fallback);
        }
    }

private:
    bool bad(const std::string& where, const std::string& what)
    {
        fprintf(stderr, "%s: %s\n", where.c_str(), what.c_str());
        return false;
    }

    // "45:35 60:55 75:100" (or "65" for a constant) → sorted points
    bool parseCurve(const std::string& text, bool constant, const std::string& where,
                    std::vector<Point>& out)
    {
        std::vector<std::string> toks;
        std::string cur;
        for (char c : text + " ")
        {
            if (c == ' ' || c == '\t' || c == ',')
            {
                if (!cur.empty())
                    toks.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }

        if (toks.empty())
            return bad(where, "empty curve");

        auto number = [](const std::string& s, float& v) -> bool
        {
            if (s.empty())
                return false;
            char* end = nullptr;
            v = strtof(s.c_str(), &end);
            return end && *end == '\0';
        };

        if (constant)
        {
            float v;
            if (toks.size() != 1 || !number(toks[0], v))
                return bad(where, "a constant source takes one value, e.g. \"65\"");
            if (v < 0 || v > 100)
                return bad(where, "percent must be 0..100");
            out.push_back({0, v});
            return true;
        }

        for (auto& t : toks)
        {
            size_t colon = t.find(':');
            float x, y;
            if (colon == std::string::npos)
                return bad(where, "\"" + t + "\": expected source:percent points, e.g. "
                                  "\"45:35 60:55 75:100\" (a bare value needs "
                                  "source \"constant\")");
            if (!number(t.substr(0, colon), x) || !number(t.substr(colon + 1), y))
                return bad(where, "\"" + t + "\": not a number pair");
            if (y < 0 || y > 100)
                return bad(where, "\"" + t + "\": percent must be 0..100");
            for (auto& p : out)
                if (p.x == x)
                    return bad(where, "two points at " + t.substr(0, colon));
            out.push_back({x, y});
        }

        for (size_t i = 1; i < out.size(); i++)
            for (size_t j = i; j > 0 && out[j].x < out[j - 1].x; j--)
                std::swap(out[j], out[j - 1]);

        return true;
    }

    bool loadHeader(const json::Value& v, const std::string& sensors, Header& h)
    {
        const std::string where = "fans." + h.key;

        if (!v.isObject())
            return bad(where, "expected an object");

        static const char* KEYS[] = {"enabled", "name", "source", "curve",
                                     "boost", "fallback"};
        for (auto& m : v.members)
        {
            bool known = false;
            for (const char* k : KEYS)
                known |= m.first == k;
            if (!known)
                return bad(where + "." + m.first, "unknown key");
        }
        for (const char* k : KEYS)
            if (!v.find(k))
                return bad(where, std::string("missing \"") + k +
                                      "\" (every header has enabled, name, "
                                      "source, curve, boost, fallback)");

        const json::Value& en = *v.find("enabled");
        if (en.type != json::Value::Type::Bool)
            return bad(where + ".enabled", "expected true or false");
        h.enabled = en.boolean;

        const json::Value& name = *v.find("name");
        if (!name.isString())
            return bad(where + ".name", "expected a string");
        h.name = name.text;

        const json::Value& src = *v.find("source");
        if (!src.isString() || src.text.empty())
            return bad(where + ".source", "expected constant, temp, cpu_load, "
                                          "gpu_load, or a hwmon chip:label / chip:pwmN");
        h.source = src.text;

        if (h.source == "constant")
            h.kind = Header::Constant;
        else if (h.source == "temp")
        {
            h.kind = Header::Temp;
            h.spec = sensors;
        }
        else if (h.source == "cpu_load")
            h.kind = Header::CpuLoad;
        else if (h.source == "gpu_load")
            h.kind = Header::GpuLoad;
        else
        {
            size_t colon = h.source.find(':');
            std::string label = colon == std::string::npos ? "" : h.source.substr(colon + 1);
            bool pwm = label.size() > 3 && label.compare(0, 3, "pwm") == 0 &&
                       label.find_first_not_of("0123456789", 3) == std::string::npos;
            if (pwm)
            {
                h.kind = Header::Pwm;
                h.spec = h.source.substr(0, colon);
                h.pwmFile = label;
            }
            else
            {
                h.kind = Header::Temp;
                h.spec = h.source;
            }
        }

        const json::Value& curve = *v.find("curve");
        if (!curve.isString() && !curve.isNumber())
            return bad(where + ".curve", "expected a string like \"45:35 60:55 75:100\"");
        if (!parseCurve(json::toString(curve), h.kind == Header::Constant,
                        where + ".curve", h.curve))
            return false;

        const json::Value& boost = *v.find("boost");
        if (boost.type == json::Value::Type::Null)
            h.boost = -1;
        else if (boost.isNumber() && boost.number >= 0 && boost.number <= 100)
            h.boost = (int)(boost.number + 0.5);
        else
            return bad(where + ".boost", "expected a percent 0..100, or null for no boost");

        const json::Value& fb = *v.find("fallback");
        if (!fb.isNumber() || fb.number < 0 || fb.number > 100)
            return bad(where + ".fallback", "expected a percent 0..100");
        h.fallback = (int)(fb.number + 0.5);

        if (h.enabled && (h.kind == Header::Temp || h.kind == Header::Pwm))
        {
            resolve(h);
            if (h.path.empty())
            {
                fprintf(stderr, "%s (%s): %s not found yet, will keep looking\n",
                        where.c_str(), h.name.c_str(), h.source.c_str());
                h.reported = true;
            }
        }

        return true;
    }

    void resolve(Header& h)
    {
        if (h.kind == Header::Temp)
            h.path = hwmon::findSensorFromSpec(h.spec);
        else if (h.kind == Header::Pwm)
            h.path = findChipFile(h.spec, h.pwmFile);
    }

    // the readings more than one header may want, once per tick
    void readShared()
    {
        bool wantCpu = false, wantGpu = false;
        for (auto& h : headers_)
        {
            wantCpu |= h.enabled && h.kind == Header::CpuLoad;
            wantGpu |= h.enabled && h.kind == Header::GpuLoad;
        }

        if (wantCpu)
        {
            unsigned long long busy, total;
            if (hwmon::readCpuCounters(busy, total))
            {
                if (cpuTotal_ && total > cpuTotal_)
                {
                    cpuLoad_ = 100.0f * (busy - cpuBusy_) / (total - cpuTotal_);
                    cpuLoadOk_ = true;
                }
                cpuBusy_ = busy;
                cpuTotal_ = total;
            }
        }

        if (wantGpu)
        {
            gpuLoadOk_ = gpu_.available();
            if (gpuLoadOk_)
                gpuLoad_ = gpu_.readPercent();
        }

        temps_.clear();
    }

    bool readInput(Header& h, float& v)
    {
        switch (h.kind)
        {
            case Header::Constant:
                v = 0;
                return true;

            case Header::CpuLoad:
                v = cpuLoad_;
                return cpuLoadOk_;

            case Header::GpuLoad:
                v = gpuLoad_;
                return gpuLoadOk_;

            case Header::Temp:
            case Header::Pwm:
                if (h.path.empty())
                {
                    resolve(h);
                    if (h.path.empty())
                        return false;
                    fprintf(stderr, "fans.%s (%s): using %s\n", h.key.c_str(),
                            h.name.c_str(), h.path.c_str());
                }

                if (h.kind == Header::Temp)
                {
                    auto it = temps_.find(h.path);
                    if (it == temps_.end())
                        it = temps_.emplace(h.path, hwmon::readTemp(h.path)).first;
                    v = it->second;
                    return true;
                }

                {
                    std::string s = hwmon::readFileLine(h.path);
                    if (s.empty())
                        return false;
                    float raw = (float)atof(s.c_str()); // 0..255
                    v = raw * 100.0f / 255.0f;
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    return true;
                }
        }

        return false;
    }

    // one header's duty this tick: read, hysteresis (temperatures), curve,
    // ramp. A header whose source can't be read runs its fallback.
    int compute(Header& h, float dt)
    {
        if (h.kind == Header::Constant)
        {
            h.out = h.curve[0].y;
            h.haveOut = true;
            return (int)(h.out + 0.5f);
        }

        float in;
        h.lastInOk = readInput(h, in);
        if (!h.lastInOk)
        {
            h.haveIn = h.haveOut = false;
            return h.fallback;
        }
        h.lastIn = in;

        // hysteresis: a temperature has to fall `hysteresis` below the value
        // the fan is running for before the fan follows it down; rises are
        // taken at once. Percent sources (loads, a mirrored pwm) skip it.
        if (h.kind == Header::Temp)
        {
            if (!h.haveIn || in > h.effIn || in < h.effIn - hysteresis_)
                h.effIn = in;
        }
        else
            h.effIn = in;
        h.haveIn = true;

        float target = h.eval(h.effIn);

        // ramp: speed-ups are immediate (cooling first), slow-downs are eased
        // at `ramp` percent per second so a curve never hunts audibly
        if (!h.haveOut || target >= h.out || ramp_ <= 0)
            h.out = target;
        else
        {
            h.out -= ramp_ * dt;
            if (h.out < target)
                h.out = target;
        }
        h.haveOut = true;

        return (int)(h.out + 0.5f);
    }

    std::vector<Header> headers_;
    int enabled_ = 0;
    float hysteresis_ = 3;
    float ramp_ = 5;
    int boostSeconds_ = 5;

    double lastTick_ = 0;
    double lastSent_ = -1e9;
    uint8_t live_[CHANNELS] = {proto::FAN_NONE, proto::FAN_NONE, proto::FAN_NONE,
                               proto::FAN_NONE, proto::FAN_NONE, proto::FAN_NONE};

    unsigned long long cpuBusy_ = 0, cpuTotal_ = 0;
    float cpuLoad_ = 0;
    bool cpuLoadOk_ = false;
    hwmon::GpuLoad gpu_;
    float gpuLoad_ = 0;
    bool gpuLoadOk_ = false;
    std::map<std::string, float> temps_; // per-tick cache by path
};
} // namespace fans
