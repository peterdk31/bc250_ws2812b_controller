#pragma once

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <fstream>
#include <sstream>
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
//
// The BLE dashboard (README "BLE remote") sees and edits this block through
// the receiver, all of it as JSON text the receiver relays without reading:
// the controller sends the config as it runs it (CMD_FAN_CONFIG, toJson) and,
// while a phone is watching, what the curves read and produce (CMD_FAN_TELEM,
// telemetryJson); a phone edit comes back as MSG_FAN_CONFIG — a partial
// object of just the fields it changed — is validated exactly like the config
// (applyJson) and, once live, is written back into the config file itself:
// only the bytes of the `fans` block are replaced (writeConfig), everything
// around it stays as the user wrote it. So the config stays the one source of
// truth — there is no second file to migrate — and a restart reads the edit
// like any other setting.
//
// The JSON shape the wire and the phone's edits use:
//
//     { "editable": true,        // the config file is writable
//       "hysteresis": 3, "ramp": 5, "boost_seconds": 5,
//       "header2": { "n": "radiator", "s": "temp",   // read-only
//                    "c": "45:35 60:55 75:100", "b": null, "f": 100 }, ... }
//
// c/b/f are curve, boost and fallback in the config's own notation; s is the
// source's kind (constant, temp, pwm, cpu_load, gpu_load). Only enabled
// headers appear. Keys are short because a GATT attribute holds 512 bytes at
// most and six headers have to fit.
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
    std::string curveText;           // the curve, canonical "x:y x:y" (or "y")
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
    // configPath is the file cfg came from — where a dashboard edit is
    // written back (see the header comment); "" = edits are refused.
    bool load(const Config& cfg, const std::string& configPath = "")
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

        sensors_ = sensors;
        block_ = *block; // the block as written, for writeConfig to update in place
        configPath_ = configPath;
        writable_ = !configPath.empty() && access(configPath.c_str(), W_OK) == 0;

        return true;
    }

    bool active() const { return enabled_ > 0; }

    // did writeConfig rewrite the config file since the last call? The main
    // loop's file watch (main.cpp) uses this to tell our own write from an
    // edit it should reload.
    bool takeWrote()
    {
        bool w = wrote_;
        wrote_ = false;
        return w;
    }

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

        readShared(now);

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

        if (watching(now))
            sendTelemetry(now, sinks);
    }

    // ---- the BLE dashboard's side (see the header comment) ----

    // the config as run, for the receiver to serve over GATT — once at startup
    // and after every applied edit
    void pushConfig(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (headers_.empty())
            return;

        std::string j = toJson();
        if (j.size() > WIRE_MAX)
        {
            // the receiver holds WIRE_MAX (a GATT attribute's ceiling) and would
            // drop this; say so once rather than leave the phone showing nothing
            if (!warnedSize_)
                fprintf(stderr, "fans: config is %zu bytes as JSON, over the dashboard's "
                                "%d — shorten header names\n", j.size(), WIRE_MAX);
            warnedSize_ = true;
            return;
        }

        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_CONFIG, (const uint8_t*)j.data(), (uint16_t)j.size());
    }

    // a msg frame from the receiver (protocol.hpp MSG_*): the phone watching,
    // or a phone edit. Main thread — the sinks' reader threads only queue.
    void onMessage(uint8_t kind, const std::vector<uint8_t>& payload, double now,
                   std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (kind == proto::MSG_FAN_WATCH)
        {
            bool on = !payload.empty() && payload[0] != 0;
            bool was = watching(now);
            watchUntil_ = on ? now + WATCH_S : 0;
            if (on && !was)
                lastTelemSent_ = -1e9; // answer a fresh watcher on the next tick
        }
        else if (kind == proto::MSG_FAN_CONFIG)
        {
            if (applyEdit(std::string(payload.begin(), payload.end()), sinks))
                pushConfig(sinks); // the answer the phone waits for
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
        fprintf(out, "  dashboard edits: %s\n",
                writable_ ? "written back to the config" : "off (config not writable)");

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
        h.curveText = curveToText(h.curve, h.kind == Header::Constant);

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

    // the readings more than one header may want, once per tick. A watching
    // phone wants all of them (the dashboard's tiles); otherwise only what
    // some curve reads is read at all.
    void readShared(double now)
    {
        bool watch = watching(now);
        bool wantCpu = watch, wantGpu = watch;
        for (auto& h : headers_)
        {
            wantCpu |= h.enabled && h.kind == Header::CpuLoad;
            wantGpu |= h.enabled && h.kind == Header::GpuLoad;
        }

        tempOk_ = false;
        if (watch)
        {
            if (tempPath_.empty())
                tempPath_ = hwmon::findSensorFromSpec(sensors_);
            if (!tempPath_.empty())
            {
                temp_ = hwmon::readTemp(tempPath_);
                tempOk_ = true;
            }
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

    // ---- dashboard helpers ----

    static const int WATCH_S = 30; // a MSG_FAN_WATCH 1 keeps telemetry flowing this long
    static const int TELEM_REFRESH_S = 5;
    static const int WIRE_MAX = 512; // a GATT attribute's ceiling = the receiver's buffer

    bool watching(double now) const { return watchUntil_ > 0 && now <= watchUntil_; }

    static const char* kindName(Header::Kind k)
    {
        switch (k)
        {
            case Header::Temp: return "temp";
            case Header::Pwm: return "pwm";
            case Header::CpuLoad: return "cpu_load";
            case Header::GpuLoad: return "gpu_load";
            default: return "constant";
        }
    }

    static std::string curveToText(const std::vector<Point>& c, bool constant)
    {
        char buf[32];
        std::string out;
        for (auto& p : c)
        {
            if (!out.empty())
                out += ' ';
            if (constant)
                snprintf(buf, sizeof buf, "%g", (double)p.y);
            else
                snprintf(buf, sizeof buf, "%g:%g", (double)p.x, (double)p.y);
            out += buf;
        }
        return out;
    }

    // a JSON string literal's body
    static std::string jsonEscape(const std::string& s)
    {
        std::string out;
        char buf[8];
        for (unsigned char ch : s)
        {
            if (ch == '"' || ch == '\\')
                out += '\\', out += (char)ch;
            else if (ch == '\n')
                out += "\\n";
            else if (ch < 0x20)
                snprintf(buf, sizeof buf, "\\u%04x", ch), out += buf;
            else
                out += (char)ch;
        }
        return out;
    }

    // the block as run, for the dashboard (see the header comment)
    std::string toJson() const
    {
        char buf[80];
        std::string j = writable_ ? "{\"editable\":true" : "{\"editable\":false";
        snprintf(buf, sizeof buf, ",\"hysteresis\":%g,\"ramp\":%g,\"boost_seconds\":%d",
                 hysteresis_, ramp_, boostSeconds_);
        j += buf;

        for (auto& h : headers_)
        {
            if (!h.enabled)
                continue;
            j += ",\"" + h.key + "\":{\"n\":\"" + jsonEscape(h.name) + "\",\"s\":\"" +
                 kindName(h.kind) + "\",\"c\":\"" + h.curveText + "\",\"b\":" +
                 (h.boost < 0 ? std::string("null") : std::to_string(h.boost)) +
                 ",\"f\":" + std::to_string(h.fallback) + "}";
        }
        return j + "}";
    }

    // what the curves see and do right now, for the dashboard's tiles and
    // cards: { "temp": 58.3, "cpu": 37, "gpu": 62,
    //          "header2": { "in": 58.3, "duty": 52 }, ... }
    // — a reading is absent when there is none, "in" when the header has no
    // input (a constant, a sensor not found), and only enabled headers appear
    std::string telemetryJson() const
    {
        char buf[64];
        std::string j = "{";
        auto add = [&](const char* s) { if (j.size() > 1) j += ','; j += s; };
        if (tempOk_)
            snprintf(buf, sizeof buf, "\"temp\":%.1f", temp_), add(buf);
        if (cpuLoadOk_)
            snprintf(buf, sizeof buf, "\"cpu\":%d", (int)(cpuLoad_ + 0.5f)), add(buf);
        if (gpuLoadOk_)
            snprintf(buf, sizeof buf, "\"gpu\":%d", (int)(gpuLoad_ + 0.5f)), add(buf);
        for (auto& h : headers_)
        {
            if (!h.enabled)
                continue;
            std::string e = "\"" + h.key + "\":{";
            if (h.kind != Header::Constant && h.lastInOk)
                snprintf(buf, sizeof buf, "\"in\":%.1f,", h.lastIn), e += buf;
            snprintf(buf, sizeof buf, "\"duty\":%d}", (int)live_[h.slot]);
            add((e + buf).c_str());
        }
        return j + "}";
    }

    void sendTelemetry(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        std::string j = telemetryJson();
        if (j == lastTelem_ && now - lastTelemSent_ < TELEM_REFRESH_S)
            return;

        lastTelem_ = j;
        lastTelemSent_ = now;
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_TELEM, (const uint8_t*)j.data(), (uint16_t)j.size());
    }

    // One header's editable fields as text — what a JSON edit reduces to, so
    // the config's own parseCurve and range checks validate every source.
    struct HeaderEdit
    {
        Header* h;
        std::string curve;
        int boost;    // -1 = none
        int fallback;
    };

    // validate a set of edits and, only if every one passes, apply them.
    // `where` prefixes the error lines. Returns false with nothing changed.
    bool applyValues(float hyst, float ramp, int boostSecs, const std::vector<HeaderEdit>& edits,
                     const std::string& where)
    {
        if (hyst < 0 || ramp < 0 || boostSecs < 0 || boostSecs > 255)
            return bad(where, "hysteresis, ramp or boost_seconds out of range");

        std::vector<std::vector<Point>> curves;
        for (auto& e : edits)
        {
            std::string at = where + "." + e.h->key;
            curves.emplace_back();
            if (!parseCurve(e.curve, e.h->kind == Header::Constant, at + ".c", curves.back()))
                return false;
            if (e.boost < -1 || e.boost > 100)
                return bad(at + ".b", "expected a percent 0..100, or null");
            if (e.fallback < 0 || e.fallback > 100)
                return bad(at + ".f", "expected a percent 0..100");
        }

        hysteresis_ = hyst;
        ramp_ = ramp;
        boostSeconds_ = boostSecs;
        for (size_t i = 0; i < edits.size(); i++)
        {
            Header& h = *edits[i].h;
            h.curve = std::move(curves[i]);
            h.curveText = curveToText(h.curve, h.kind == Header::Constant);
            h.boost = edits[i].boost;
            h.fallback = edits[i].fallback;
            // haveOut stays: the ramp eases from the old duty to the new curve
        }
        return true;
    }

    // apply a phone's partial edit (the shape in the header comment): every
    // key optional, a header's c/b/f each optional (the rest keeps its value),
    // the read-only keys ignored, anything else an error. Validated as a whole
    // by applyValues.
    bool applyJson(const json::Value& root, const std::string& where)
    {
        float hyst = hysteresis_, ramp = ramp_;
        int boostSecs = boostSeconds_;
        std::vector<HeaderEdit> edits;

        for (auto& m : root.members)
        {
            const std::string& k = m.first;
            const json::Value& v = m.second;
            if (k == "editable")
                continue;
            if (k == "hysteresis" || k == "ramp" || k == "boost_seconds")
            {
                if (!v.isNumber())
                    return bad(where + "." + k, "expected a number");
                if (k == "hysteresis")
                    hyst = (float)v.number;
                else if (k == "ramp")
                    ramp = (float)v.number;
                else
                    boostSecs = (int)v.number;
            }
            else if (k.rfind("header", 0) == 0 && v.isObject())
            {
                Header* h = nullptr;
                for (auto& o : headers_)
                    if (o.key == k)
                        h = &o;
                if (!h)
                    return bad(where + "." + k, "no such header in the config");

                HeaderEdit e{h, h->curveText, h->boost, h->fallback};
                for (auto& f : v.members)
                {
                    const std::string& fk = f.first;
                    const json::Value& fv = f.second;
                    if (fk == "n" || fk == "s")
                        continue;
                    if (fk == "c" && (fv.isString() || fv.isNumber()))
                        e.curve = json::toString(fv);
                    else if (fk == "b" && fv.type == json::Value::Type::Null)
                        e.boost = -1;
                    else if (fk == "b" && fv.isNumber())
                        e.boost = (int)(fv.number + 0.5);
                    else if (fk == "f" && fv.isNumber())
                        e.fallback = (int)(fv.number + 0.5);
                    else
                        return bad(where + "." + k + "." + fk, "not understood");
                }
                edits.push_back(e);
            }
            else
                return bad(where + "." + k, "unknown key");
        }

        return applyValues(hyst, ramp, boostSecs, edits, where);
    }

    // a phone edit: parse, apply, write it into the config, and re-push the
    // standalone part if that moved. False (with a journal line) changes
    // nothing.
    bool applyEdit(const std::string& text, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        const std::string where = "fans (dashboard edit)";

        if (!writable_ || headers_.empty())
            return bad(where, "refused — the config file is not writable, or has no fans block");

        json::Value root;
        std::string err;
        if (!json::parse(text, root, err) || !root.isObject())
            return bad(where, "not a JSON object: " + err);

        std::string before = standaloneKey();
        if (!applyJson(root, where))
        {
            fprintf(stderr, "fans: dashboard edit refused — nothing changed\n");
            return false;
        }

        fprintf(stderr, "fans: live edit applied from the dashboard\n");
        writeConfig();
        if (standaloneKey() != before)
            pushStandalone(sinks);
        lastSent_ = -1e9; // send the new duties on the next tick, changed or not
        return true;
    }

    // the receiver's standalone settings as one comparable string
    std::string standaloneKey() const
    {
        std::string k = std::to_string(boostSeconds_);
        for (auto& h : headers_)
            k += "|" + std::to_string(h.boost) + ":" + std::to_string(h.fallback);
        return k;
    }

    // ---- writing an edit back into the config file ----

    // a mutable member of an object, appended when absent
    static json::Value& member(json::Value& obj, const std::string& key)
    {
        for (auto& m : obj.members)
            if (m.first == key)
                return m.second;
        obj.members.emplace_back(key, json::Value());
        return obj.members.back().second;
    }

    static json::Value jsonNumber(double v)
    {
        json::Value n;
        n.type = json::Value::Type::Number;
        n.number = v;
        return n;
    }

    static json::Value jsonString(const std::string& v)
    {
        json::Value n;
        n.type = json::Value::Type::String;
        n.text = v;
        return n;
    }

    // pretty-print a value the way the config is written: four-space
    // indents, one member or item per line. `depth` is the nesting of the
    // value's own opening bracket (the fans block sits at 1).
    static void writeJson(const json::Value& v, int depth, std::string& out)
    {
        using T = json::Value::Type;
        const std::string pad(4 * depth, ' '), padIn(4 * (depth + 1), ' ');
        char buf[32];

        switch (v.type)
        {
            case T::Null: out += "null"; break;
            case T::Bool: out += v.boolean ? "true" : "false"; break;
            case T::Number:
                if (v.number == (double)(long long)v.number && fabs(v.number) < 1e15)
                    snprintf(buf, sizeof buf, "%lld", (long long)v.number);
                else
                    snprintf(buf, sizeof buf, "%.10g", v.number);
                out += buf;
                break;
            case T::String: out += "\"" + jsonEscape(v.text) + "\""; break;
            case T::Array:
                if (v.items.empty()) { out += "[]"; break; }
                out += "[\n";
                for (size_t i = 0; i < v.items.size(); i++)
                {
                    out += padIn;
                    writeJson(v.items[i], depth + 1, out);
                    out += i + 1 < v.items.size() ? ",\n" : "\n";
                }
                out += pad + "]";
                break;
            case T::Object:
                if (v.members.empty()) { out += "{}"; break; }
                out += "{\n";
                for (size_t i = 0; i < v.members.size(); i++)
                {
                    out += padIn + "\"" + jsonEscape(v.members[i].first) + "\": ";
                    writeJson(v.members[i].second, depth + 1, out);
                    out += i + 1 < v.members.size() ? ",\n" : "\n";
                }
                out += pad + "}";
                break;
        }
    }

    // the end of the JSON value starting at t[i] (a bracketed value, a string
    // or a bare scalar), respecting strings and escapes
    static size_t skipValue(const std::string& t, size_t i)
    {
        auto skipString = [&](size_t j) {
            for (j++; j < t.size(); j++)
            {
                if (t[j] == '\\') j++;
                else if (t[j] == '"') return j + 1;
            }
            return j;
        };
        if (t[i] == '"')
            return skipString(i);
        if (t[i] == '{' || t[i] == '[')
        {
            int depth = 0;
            for (; i < t.size(); i++)
            {
                if (t[i] == '"') { i = skipString(i) - 1; continue; }
                if (t[i] == '{' || t[i] == '[') depth++;
                else if ((t[i] == '}' || t[i] == ']') && --depth == 0) return i + 1;
            }
            return i;
        }
        while (i < t.size() && !strchr(",}] \t\r\n", t[i]))
            i++;
        return i;
    }

    // the byte span [start, end) of the top-level "fans" member's value in
    // the config's text. The daemon parsed this same text at startup, so only
    // a file changed underneath us can make this fail.
    static bool fansSpan(const std::string& t, size_t& start, size_t& end)
    {
        int depth = 0;
        for (size_t i = 0; i < t.size(); i++)
        {
            char c = t[i];
            if (c == '"')
            {
                size_t e = skipValue(t, i);
                std::string str = t.substr(i + 1, e - i - 2);
                size_t k = e;
                while (k < t.size() && isspace((unsigned char)t[k])) k++;
                if (depth == 1 && k < t.size() && t[k] == ':' && str == "fans")
                {
                    for (k++; k < t.size() && isspace((unsigned char)t[k]); k++) {}
                    start = k;
                    end = skipValue(t, k);
                    return end > start;
                }
                i = e - 1;
            }
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') depth--;
        }
        return false;
    }

    // fold the running values into the block as parsed (every key the user
    // wrote, in their order) and splice it over the block's bytes in the
    // config file; the rest of the file is untouched. A failure is said and
    // leaves the edit live until the next restart, and the dashboard turns
    // read-only so the phone stops offering saves that can't stick.
    void writeConfig()
    {
        member(block_, "hysteresis") = jsonNumber(hysteresis_);
        member(block_, "ramp") = jsonNumber(ramp_);
        member(block_, "boost_seconds") = jsonNumber(boostSeconds_);
        for (auto& h : headers_)
        {
            json::Value& hv = member(block_, h.key);
            if (!hv.isObject())
                continue;
            member(hv, "curve") = jsonString(h.curveText);
            member(hv, "boost") = h.boost < 0 ? json::Value() : jsonNumber(h.boost);
            member(hv, "fallback") = jsonNumber(h.fallback);
        }

        std::string why;
        std::ifstream in(configPath_);
        std::stringstream buf;
        if (in.is_open())
            buf << in.rdbuf();
        std::string text = buf.str();
        size_t a = 0, b = 0;
        if (text.empty())
            why = "can't read it";
        else if (!fansSpan(text, a, b))
            why = "no top-level \"fans\" block found — was the file changed under the daemon?";

        // the first rewrite of this daemon's life keeps the file as it found
        // it, beside it, as insurance against this very code — a tuned config
        // is the one thing on the box that is hard to recreate
        if (why.empty() && !backedUp_)
        {
            std::string bak = configPath_ + ".bak";
            FILE* bf = fopen(bak.c_str(), "w");
            if (bf)
            {
                fwrite(text.data(), 1, text.size(), bf);
                fclose(bf);
            }
            backedUp_ = true;
        }

        std::string block;
        writeJson(block_, 1, block);
        text = text.substr(0, a) + block + text.substr(b);

        std::string tmp = configPath_ + ".tmp";
        struct stat st;
        bool ok = why.empty();
        FILE* f = ok ? fopen(tmp.c_str(), "w") : nullptr;
        if (ok && !f)
            why = strerror(errno), ok = false;
        if (ok)
        {
            ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0 &&
                 fsync(fileno(f)) == 0;
            ok = fclose(f) == 0 && ok;
            if (ok && stat(configPath_.c_str(), &st) == 0)
                chmod(tmp.c_str(), st.st_mode & 07777);
            if (ok && rename(tmp.c_str(), configPath_.c_str()) != 0)
                ok = false;
            if (!ok)
            {
                why = strerror(errno);
                unlink(tmp.c_str());
            }
        }

        if (!ok)
        {
            fprintf(stderr, "fans: can't write the edit to %s: %s — it runs until "
                            "restart; dashboard now read-only\n",
                    configPath_.c_str(), why.c_str());
            writable_ = false;
            return;
        }
        wrote_ = true;
        fprintf(stderr, "fans: %s updated\n", configPath_.c_str());
    }

    std::vector<Header> headers_;
    int enabled_ = 0;
    float hysteresis_ = 3;
    float ramp_ = 5;
    int boostSeconds_ = 5;

    std::string sensors_;    // the top-level `sensors` spec (telemetry temp)
    json::Value block_;      // the fans block as written (writeConfig updates it)
    std::string configPath_; // the file it came from
    bool writable_ = false;  // ...and whether edits can be written back to it
    bool wrote_ = false;     // writeConfig succeeded since the last takeWrote()
    bool backedUp_ = false;  // <config>.bak written this run

    double watchUntil_ = 0;    // a phone watches until this time (0 = none)
    std::string tempPath_;
    float temp_ = 0;
    bool tempOk_ = false;
    std::string lastTelem_;
    double lastTelemSent_ = -1e9;
    bool warnedSize_ = false;

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
