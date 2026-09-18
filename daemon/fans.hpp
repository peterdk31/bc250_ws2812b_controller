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
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config_edit.hpp"
#include "config_loader.hpp"
#include "fancurve.hpp"
#include "fanwire.hpp"
#include "hwmon.hpp"
#include "protocol.hpp"
#include "sink.hpp"

// The daemon's half of the fan controller (README "Fans"). The receiver
// drives the PWM headers; this side decides what they should run, from the
// config's "fans" block:
//
//     "fans": {
//         "header1": { "name": "pump", "source": "fallback",
//                      "boost": 100, "boost_seconds": 5, "fallback": 65 },
//         "header2": { ... "source": "temp", "curve": "45:35 60:55 75:100",
//                      "hysteresis": 3, "ramp": 5, "boost": null, "fallback": 100 },
//         "header3": { ... "source": "gpio:0", "curve": "0:25 100:80",
//                      "ramp": 0, "boost": null, "fallback": 100 },
//         ...
//     }
//
// Every header has name, source, boost and fallback, plus a curve for every
// source but "fallback", and optionally its tunings — hysteresis, ramp,
// boost_seconds (TUNINGS below: each has a default and applies to some
// headers only; on the others it is refused). There is nothing global: a
// board's own fan header mirrored over pwm wants no ramp at all while the
// radiator next to it wants one, so each header says for itself. A header
// listed here is driven: its curve runs, its
// output is wired at flash time. To take one out of service, delete its
// block (and reflash fancfg); to stop its fan, give it source "fallback" and
// a fallback of 0. There is no enable flag — one existed, and meant two
// things at once (wiring at flash time, curve at runtime), so a phone could
// switch on a header the receiver had no pin for; the key is now a startup
// error. `source` is what the curve reads, and WHERE the curve runs follows
// from what can read it:
//
//   fallback     nothing: the header runs its fallback value, always
//   gpio:N       the duty of a PWM signal on the RECEIVER's GPIO N (the
//                BC-250's own fan header, wired over) — the receiver samples
//                it and runs the curve itself, so this works with no daemon
//                and the machine off. x is 0..100 %
//   temp         the top-level `sensors` pick, °C            (this daemon)
//   chip:label   any hwmon temperature, °C                   (this daemon)
//   pmbus:CPU VRM / pmbus:GPU VRM
//                the BC-250's VRM controller over I2C, °C   (this daemon)
//   file:/path   a file holding one temperature, °C         (this daemon)
//   chip:pwmN    a hwmon pwm output, read as 0..100 %        (this daemon)
//   cpu_load / gpu_load   0..100 %                           (this daemon)
//
// A daemon-evaluated ("host") curve's output goes out as CMD_FAN_LIVE whole
// percents on the rules' 0.5 s tick, sharing the reads the rule conditions
// already do — when something changed, plus a refresh every few seconds so
// the receiver can treat a silence as "the daemon is gone" and fall back,
// which is also why a live duty is never persisted on the receiver. A
// fallback or gpio header is never the daemon's to drive: its live slot is
// always FAN_NONE.
//
// `curve` is "x:percent" points (up to fancurve::MAX_POINTS), linear between
// and flat beyond the ends; the x unit is the source's. `boost` is the duty
// for the first boost_seconds after the host powers on (null = sits it out)
// and `fallback` what the receiver runs whenever nothing else drives the
// header. Those, the ramp, and every header's source kind (with a gpio
// header's pin and curve) are the receiver's standalone settings — one
// fanwire::Header record per header: pushed at startup and after any edit
// that moves them (CMD_FAN_STANDALONE, common/protocol.hpp) and also baked into its fancfg
// partition at flash time from this same block (tools/fancfg.py). They are
// pushed for every header in this block — the config is the one source of
// truth while a daemon is connected, and a value dialled on the receiver
// from the phone while no daemon ran is overwritten by it.
//
// The BLE dashboard (README "BLE remote") sees and edits this block through
// the receiver, all of it as JSON text the receiver relays without reading:
// the controller sends the config as it runs it (CMD_FAN_CONFIG, toJson) and,
// while a phone is watching, what the curves read and produce (CMD_FAN_TELEM,
// telemetryJson); a phone edit comes back as MSG_FAN_CONFIG — a partial
// object of just the fields it changed — is validated exactly like the config
// (applyJson) and, once live, is written back into the config file itself:
// only the bytes of the `fans` block are replaced (writeConfig, through
// daemon/config_edit.hpp), everything around it stays as the user wrote it.
// So the config stays the one source of truth — there is no second file to
// migrate — and a restart reads the edit like any other setting.
//
// The JSON shape the wire and the phone's edits use:
//
//     { "editable": true,        // the config file is writable
//       "header1": { "n": "pump", "s": "fallback", "b": 100, "t": 5, "f": 65 },
//       "header2": { "n": "radiator", "s": "temp",
//                    "c": "45:35 60:55 75:100", "f": 100, "h": 3, "r": 5 }, ... }
//
// s is the source exactly as the config spells it, c/b/f are curve, boost
// and fallback in the config's own notation (c absent for a fallback source,
// b absent = null, no boost), n the name, h/r/t the tunings (TUNINGS' short
// keys; one is absent when it doesn't apply to the header or sits at its
// default, which the page knows). All of them are editable; a phone that
// changes s sends the curve for the new source in the same edit. Keys are
// short because a GATT attribute holds 512 bytes at most and six headers
// have to fit — pushConfig says so in the journal when they don't.
namespace fans
{
static const int CHANNELS = proto::FAN_CHANNELS;
static const double REFRESH_S = 5.0; // resend unchanged live duties this often
                                     // (the receiver drops them after 15 s of
                                     // silence — see firmware fan.cpp)
static const int MAX_GPIO = 48;      // the highest GPIO on any target (S3);
                                     // the flasher and the receiver know the
                                     // real chip and check the pin properly
static const float DEFAULT_HYSTERESIS = 3; // °C; the ramp's and boost length's
                                           // defaults are the wire's (protocol.hpp)

static_assert(proto::FAN_CURVE_POINTS == fancurve::MAX_POINTS,
              "the wire's point count is the evaluator's");

using fancurve::Point;

struct Header
{
    int slot = 0;                    // 0-based; header1 is slot 0
    std::string key;                 // "header1"
    std::string name;
    std::string source;              // as written

    enum Kind { Fallback, Gpio, Temp, Pwm, CpuLoad, GpuLoad } kind = Fallback;
    int gpio = -1;                   // Gpio: the receiver's input pin
    std::vector<Point> curve;        // sorted by x; empty for Fallback
    std::string curveText;           // the curve, canonical "x:y x:y"
    int boost = -1;                  // percent, -1 = null (no boost)
    int fallback = 100;
    // the tunings (TUNINGS below); one that doesn't apply to this header's
    // kind holds its default and is never written out
    float hysteresis = DEFAULT_HYSTERESIS;       // °C a temperature must fall before the fan follows
    float ramp = proto::FAN_DEFAULT_RAMP;        // percent per second on the way down, 0 = at once
    float boostSecs = proto::FAN_DEFAULT_BOOST_SECS;

    // runtime
    std::string spec;                // hwmon candidates (Temp) / chip (Pwm)
    std::string pwmFile;             // "pwm1" (Pwm)
    std::string path;                // resolved sysfs file, "" = not yet
    bool reported = false;           // "no sensor yet" said once
    bool lost = false;               // the resolved sensor stopped reading (said once)
    bool haveIn = false;
    float effIn = 0;                 // hysteresis-filtered input
    bool haveOut = false;
    float out = 0;                   // ramped output
    float lastIn = 0;                // last raw reading (for --fan-status)
    bool lastInOk = false;

    // the daemon evaluates this header's curve (the receiver runs the rest)
    bool hostRun() const { return kind != Fallback && kind != Gpio; }

    float eval(float x) const
    {
        return curve.empty() ? fallback : fancurve::eval(curve.data(), (int)curve.size(), x);
    }

    // the receiver's record of this header (protocol.hpp CMD_FAN_STANDALONE)
    fanwire::Header wire() const
    {
        fanwire::Header w;
        w.fallback = (uint8_t)fallback;
        w.boost = boost < 0 ? fanwire::NONE : (uint8_t)boost;
        w.boostSecs = (uint8_t)(boostSecs + 0.5f);
        w.ramp = (uint8_t)(ramp + 0.5f);
        w.kind = kind == Fallback ? proto::FAN_KIND_FALLBACK
               : kind == Gpio     ? proto::FAN_KIND_GPIO
                                  : proto::FAN_KIND_HOST;
        if (kind == Gpio)
        {
            w.gpio = (uint8_t)gpio;
            w.npts = (uint8_t)curve.size();
            for (size_t i = 0; i < curve.size(); i++)
            {
                w.pts[i][0] = (uint8_t)(curve[i].x + 0.5f);
                w.pts[i][1] = (uint8_t)(curve[i].y + 0.5f);
            }
        }
        return w;
    }
};

// A header's tunings: optional numbers with a default, each meaningful for
// some headers only — and refused on the others, so a hysteresis on a load
// header is a typo caught at startup rather than a number that silently does
// nothing. This one table drives the config parser, the dashboard's JSON in
// both directions, the write-back into the file and --fan-status.
struct Tuning
{
    const char* key;      // the config's
    const char* shortKey; // the dashboard's
    const char* range;    // what a value outside lo..hi is told
    float lo, hi;
    float Header::* field;
    bool (*applies)(const Header&);
    const char* onlyFor;  // "...only applies to <onlyFor>"
    float dflt;
};
static const Tuning TUNINGS[] = {
    {"hysteresis", "h", "a number of °C, 0 or more", 0, 1e9f, &Header::hysteresis,
     [](const Header& h) { return h.kind == Header::Temp; }, "a temperature source", DEFAULT_HYSTERESIS},
    {"ramp", "r", "percent per second, 0..255 (0 = at once)", 0, 255, &Header::ramp,
     [](const Header& h) { return h.kind != Header::Fallback; }, "a source with a curve", proto::FAN_DEFAULT_RAMP},
    {"boost_seconds", "t", "seconds, 0..255", 0, 255, &Header::boostSecs,
     [](const Header& h) { return h.boost >= 0; }, "a header with a boost", proto::FAN_DEFAULT_BOOST_SECS},
};
static const int TUNING_COUNT = sizeof TUNINGS / sizeof *TUNINGS;

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
    // the top-level config block this module owns. Nothing in it feeds the
    // strip, so a reload confined to it leaves the running effect alone
    // (main.cpp asks each module for its block rather than knowing the names).
    static constexpr const char* BLOCK = "fans";

    // read and validate the "fans" block. Returns false (having said what is
    // wrong, "fans.header2.curve: ...") on a bad block, so the daemon can
    // refuse to start the way it does for a bad rule; true with no block or no
    // header, in which case active() is false and nothing is ever sent.
    // writer is the config file's editor (config_edit.hpp), where a dashboard
    // edit is written back (see the header comment); null = edits are refused.
    bool load(const Config& cfg, cfgedit::Writer* writer = nullptr)
    {
        const json::Value* block = cfg.root().find(BLOCK);

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

            if (k == "hysteresis" || k == "ramp" || k == "boost_seconds")
                return bad("fans." + k, "moved: this is each header's own setting now — put it "
                                        "in the fans.headerN block it belongs to (README \"Fans\")");
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

        sensors_ = sensors;
        block_ = *block; // the block as written, for writeConfig to update in place
        writer_ = writer;

        return true;
    }

    bool active() const { return !headers_.empty(); }

    bool writable() const { return writer_ && writer_->writable(); }

    // the receiver's standalone settings (protocol.hpp CMD_FAN_STANDALONE) —
    // once, at startup, and again when an edit moves them. Every header in
    // the block: the config wins over whatever the receiver held (see the
    // header comment)
    void pushStandalone(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (!active())
            return;

        uint8_t p[proto::FAN_STANDALONE_LEN];
        standaloneBlob(p);
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_STANDALONE, p, sizeof p);
    }

    // one evaluation: read every host-run header's source, run the curves,
    // and send the live duties if they changed (or the refresh is due). Call
    // it on the rules tick; it costs nothing when there are no headers and no
    // phone is watching (a watcher still gets the host tiles' readings).
    void tick(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        bool watch = watching(now);
        if (!active() && !watch)
            return;

        float dt = lastTick_ > 0 ? (float)(now - lastTick_) : 0.5f;
        lastTick_ = now;

        readShared(now);

        uint8_t p[CHANNELS];
        memset(p, proto::FAN_NONE, sizeof p);

        for (auto& h : headers_)
            p[h.slot] = (uint8_t)compute(h, dt);

        bool changed = memcmp(p, live_, sizeof p) != 0;

        if (changed || (active() && now - lastSent_ >= REFRESH_S))
        {
            memcpy(live_, p, sizeof p);
            lastSent_ = now;

            for (auto& s : sinks)
                s->sendCommand(proto::CMD_FAN_LIVE, p, sizeof p);
        }

        if (watch)
        {
            sendTelemetry(now, sinks);
            sendSensors(now, sinks);
        }
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
                                "%d — shorten header names or sources\n", j.size(), WIRE_MAX);
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
            {
                lastTelemSent_ = lastSensorsSent_ = -1e9; // answer a fresh watcher on the next tick
                pwmSplit_.clear(); // the pwm outputs start out as one entry again
            }
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

        fprintf(out, "fans: dashboard edits: %s\n",
                writable() ? "written back to the config" : "off (config not writable)");
        fprintf(out, "  sensors a header could follow (the phone's picker): %s\n", sensorsJson().c_str());

        for (auto& h : headers_)
        {
            fprintf(out, "  %s %-10s", h.key.c_str(), h.name.c_str());

            if (h.kind == Header::Fallback)
                fprintf(out, "  fallback (the receiver runs it at %d%%)", h.fallback);
            else if (h.kind == Header::Gpio)
                fprintf(out, "  %s \"%s\" (the receiver reads the pin and runs the curve)",
                        h.source.c_str(), h.curveText.c_str());
            else
            {
                fprintf(out, "  %s", h.source.c_str());
                if (h.kind == Header::Temp || h.kind == Header::Pwm)
                    fprintf(out, " -> %s", h.path.empty() ? "(not found)" : h.path.c_str());
                if (h.lastInOk)
                    fprintf(out, " = %g%s", h.lastIn, h.kind == Header::Temp ? " °C" : " %");
                else
                    fprintf(out, " = (no reading)");
                fprintf(out, "  -> %d%%", (int)live_[h.slot]);
            }

            fprintf(out, "   boost %s, fallback %d%%",
                    h.boost < 0 ? "none" : (std::to_string(h.boost) + "%").c_str(),
                    h.fallback);
            for (auto& t : TUNINGS)
                if (t.applies(h))
                    fprintf(out, ", %s %g", t.key, h.*t.field);
            fprintf(out, "\n");
        }
    }

private:
    bool bad(const std::string& where, const std::string& what)
    {
        fprintf(stderr, "%s: %s\n", where.c_str(), what.c_str());
        return false;
    }

    static const char* SOURCE_HELP()
    {
        return "expected fallback, gpio:N, temp, cpu_load, gpu_load, a hwmon "
               "chip:label / chip:pwmN, pmbus:CPU VRM / pmbus:GPU VRM, or file:/path";
    }

    // what a source string means: kind, and for gpio the pin, for hwmon
    // sources the spec. "" when it parses, else what is wrong with it. Only
    // the source fields of h are touched.
    static std::string parseSource(const std::string& src, const std::string& sensors,
                                   Header& h)
    {
        h.gpio = -1;
        h.spec.clear();
        h.pwmFile.clear();

        if (src.empty())
            return SOURCE_HELP();

        if (src == "constant")
            return "renamed: a fixed speed is source \"fallback\" — the header runs its "
                   "fallback value and takes no curve (move the constant into fallback)";

        if (src == "fallback")
        {
            h.kind = Header::Fallback;
            return "";
        }

        if (src == "temp")
        {
            h.kind = Header::Temp;
            h.spec = sensors;
            return "";
        }

        if (src == "cpu_load")
        {
            h.kind = Header::CpuLoad;
            return "";
        }

        if (src == "gpu_load")
        {
            h.kind = Header::GpuLoad;
            return "";
        }

        size_t colon = src.find(':');
        if (colon == std::string::npos)
            return SOURCE_HELP();

        std::string chip = src.substr(0, colon);
        std::string label = src.substr(colon + 1);

        if (chip == "gpio")
        {
            char* end = nullptr;
            long n = label.empty() ? -1 : strtol(label.c_str(), &end, 10);
            if (label.empty() || *end || n < 0 || n > MAX_GPIO)
                return "gpio:N names a receiver GPIO, 0.." + std::to_string(MAX_GPIO) +
                       " (the flasher checks it against the chip and the pins other "
                       "features use)";
            h.kind = Header::Gpio;
            h.gpio = (int)n;
            return "";
        }

        // the two sources outside hwmon (hwmon.hpp): a spec that could never
        // resolve is a typo, not a sensor to keep looking for. A comma list
        // of candidates is checked one by one.
        for (auto& c : hwmon::split(src, ','))
        {
            size_t k = c.find(':');
            std::string cchip = c.substr(0, k), clabel = k == std::string::npos ? "" : c.substr(k + 1);
            if (cchip == "pmbus" && pmbus::railOf(clabel) < 0)
            {
                std::string rails;
                for (int i = 0; i < pmbus::RAIL_COUNT; i++)
                    rails += std::string(i ? " or pmbus:" : "pmbus:") + pmbus::RAILS[i].label;
                return "the VRM controller's rails are " + rails;
            }
            if (cchip == "file" && (clabel.empty() || clabel[0] != '/'))
                return "file:/path names a file holding one temperature (millidegrees or degrees)";
        }

        bool pwm = label.size() > 3 && label.compare(0, 3, "pwm") == 0 &&
                   label.find_first_not_of("0123456789", 3) == std::string::npos;
        if (pwm)
        {
            h.kind = Header::Pwm;
            h.spec = chip;
            h.pwmFile = label;
        }
        else
        {
            h.kind = Header::Temp;
            h.spec = src;
        }
        return "";
    }

    // "45:35 60:55 75:100" → sorted points, for a header of kind `kind`
    bool parseCurve(const std::string& text, Header::Kind kind, const std::string& where,
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
        if ((int)toks.size() > fancurve::MAX_POINTS)
            return bad(where, "at most " + std::to_string(fancurve::MAX_POINTS) + " points");

        auto number = [](const std::string& s, float& v) -> bool
        {
            if (s.empty())
                return false;
            char* end = nullptr;
            v = strtof(s.c_str(), &end);
            return end && *end == '\0';
        };

        for (auto& t : toks)
        {
            size_t colon = t.find(':');
            float x, y;
            if (colon == std::string::npos)
                return bad(where, "\"" + t + "\": expected source:percent points, e.g. "
                                  "\"45:35 60:55 75:100\" (a fixed speed is source "
                                  "\"fallback\" with no curve)");
            if (!number(t.substr(0, colon), x) || !number(t.substr(colon + 1), y))
                return bad(where, "\"" + t + "\": not a number pair");
            if (y < 0 || y > 100)
                return bad(where, "\"" + t + "\": percent must be 0..100");
            if (kind == Header::Gpio && (x < 0 || x > 100 || x != floorf(x)))
                return bad(where, "\"" + t + "\": a gpio curve's input is whole percents "
                                  "0..100 (it travels to the receiver as bytes)");
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

        static const char* KEYS[] = {"name", "source", "curve", "boost", "fallback"};
        for (auto& m : v.members)
        {
            if (m.first == "enabled")
                return bad(where + ".enabled",
                           "retired — a header listed here is always driven; delete "
                           "the header's block to drop it, or give it source "
                           "\"fallback\" with a fallback of 0 to stop the fan");
            bool known = false;
            for (const char* k : KEYS)
                known |= m.first == k;
            for (auto& t : TUNINGS)
                known |= m.first == t.key;
            if (!known)
                return bad(where + "." + m.first, "unknown key");
        }
        for (const char* k : KEYS)
            if (strcmp(k, "curve") != 0 && !v.find(k))
                return bad(where, std::string("missing \"") + k +
                                      "\" (every header has name, source, boost, "
                                      "fallback, and a curve unless the source is "
                                      "fallback)");

        const json::Value& name = *v.find("name");
        if (!name.isString())
            return bad(where + ".name", "expected a string");
        h.name = name.text;

        const json::Value& src = *v.find("source");
        if (!src.isString())
            return bad(where + ".source", SOURCE_HELP());
        h.source = src.text;
        std::string e = parseSource(h.source, sensors, h);
        if (!e.empty())
            return bad(where + ".source", e);

        const json::Value* curve = v.find("curve");
        if (h.kind == Header::Fallback)
        {
            if (curve)
                return bad(where + ".curve", "a fallback source takes no curve — the header "
                                             "runs its fallback value; delete this key");
        }
        else
        {
            if (!curve)
                return bad(where, "missing \"curve\" (source:percent points, e.g. "
                                  "\"45:35 60:55 75:100\")");
            if (!curve->isString())
                return bad(where + ".curve", "expected a string like \"45:35 60:55 75:100\"");
            if (!parseCurve(curve->text, h.kind, where + ".curve", h.curve))
                return false;
            h.curveText = curveToText(h.curve);
        }

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

        for (auto& t : TUNINGS)
        {
            const json::Value* tv = v.find(t.key);
            if (!tv)
                continue;
            if (!tv->isNumber() || tv->number < t.lo || tv->number > t.hi)
                return bad(where + "." + t.key, std::string("expected ") + t.range);
            if (!t.applies(h))
                return bad(where + "." + t.key, std::string(t.key) + " only applies to " + t.onlyFor +
                                                    " — delete this key");
            h.*t.field = (float)tv->number;
        }

        if (h.kind == Header::Temp || h.kind == Header::Pwm)
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
            wantCpu |= h.kind == Header::CpuLoad;
            wantGpu |= h.kind == Header::GpuLoad;
        }

        tempOk_ = false;
        if (watch)
        {
            if (tempPath_.empty())
                tempPath_ = hwmon::findSensorFromSpec(sensors_);
            if (!tempPath_.empty())
            {
                tempOk_ = hwmon::readTempOk(tempPath_, temp_); // the tile shows "—" for a sensor that isn't reading
                if (!tempOk_ && !hwmon::fileExists(tempPath_))
                    tempPath_.clear();
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
            case Header::Fallback:
            case Header::Gpio:
                return false; // the receiver's, never read here

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
                    // per tick, one read per path; NaN = no usable reading
                    auto it = temps_.find(h.path);
                    if (it == temps_.end())
                    {
                        float t;
                        it = temps_.emplace(h.path, hwmon::readTempOk(h.path, t) ? t : NAN).first;
                    }
                    if (std::isnan(it->second))
                    {
                        // an unplugged thermistor reads 0, a gone chip reads
                        // nothing: either way the header runs its fallback
                        // rather than a curve fed a temperature nobody measured
                        if (!h.lost)
                            fprintf(stderr, "fans.%s (%s): %s gives no usable reading — running the "
                                            "fallback until it does\n", h.key.c_str(), h.name.c_str(), h.path.c_str());
                        h.lost = true;
                        if (!hwmon::fileExists(h.path))
                            h.path.clear(); // gone: look it up again — it may return under another hwmon number
                        return false;
                    }
                    if (h.lost)
                        fprintf(stderr, "fans.%s (%s): %s is reading again\n", h.key.c_str(), h.name.c_str(), h.path.c_str());
                    h.lost = false;
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

    // one header's live duty this tick: read, hysteresis (temperatures),
    // curve, ramp. A header whose source can't be read runs its fallback; a
    // header the receiver runs itself is FAN_NONE — not this side's to drive.
    int compute(Header& h, float dt)
    {
        if (!h.hostRun())
            return proto::FAN_NONE;

        float in;
        h.lastInOk = readInput(h, in);
        if (!h.lastInOk)
        {
            h.haveIn = h.haveOut = false;
            return h.fallback;
        }
        h.lastIn = in;

        // hysteresis: a temperature has to fall the header's `hysteresis`
        // below the value the fan is running for before the fan follows it
        // down; rises are taken at once. Percent sources (loads, a mirrored
        // pwm) skip it.
        if (h.kind == Header::Temp)
        {
            if (!h.haveIn || in > h.effIn || in < h.effIn - h.hysteresis)
                h.effIn = in;
        }
        else
            h.effIn = in;
        h.haveIn = true;

        h.out = fancurve::ramp(h.eval(h.effIn), h.out, h.haveOut, h.ramp, dt);
        h.haveOut = true;

        return (int)(h.out + 0.5f);
    }

    // ---- dashboard helpers ----

    static const int WATCH_S = 30; // a MSG_FAN_WATCH 1 keeps telemetry flowing this long
    static const int TELEM_REFRESH_S = 5;
    static const int SENSORS_REFRESH_S = 5; // the catalogue's pace while watched
    static const int WIRE_MAX = 512; // a GATT attribute's ceiling = the receiver's buffer

    bool watching(double now) const { return watchUntil_ > 0 && now <= watchUntil_; }

    static std::string curveToText(const std::vector<Point>& c)
    {
        char buf[32];
        std::string out;
        for (auto& p : c)
        {
            if (!out.empty())
                out += ' ';
            snprintf(buf, sizeof buf, "%g:%g", (double)p.x, (double)p.y);
            out += buf;
        }
        return out;
    }

    // the block as run, for the dashboard (see the header comment)
    std::string toJson() const
    {
        char buf[40];
        std::string j = writable() ? "{\"editable\":true" : "{\"editable\":false";

        for (auto& h : headers_)
        {
            j += ",\"" + h.key + "\":{\"n\":\"" + cfgedit::escape(h.name) + "\",\"s\":\"" +
                 cfgedit::escape(h.source) + "\"";
            if (h.kind != Header::Fallback)
                j += ",\"c\":\"" + h.curveText + "\"";
            j += (h.boost < 0 ? std::string() : ",\"b\":" + std::to_string(h.boost)) +
                 ",\"f\":" + std::to_string(h.fallback);
            // a tuning travels when it applies and moved off its default; the
            // page fills in the defaults (the wire's, protocol.hpp)
            for (auto& t : TUNINGS)
                if (t.applies(h) && h.*t.field != t.dflt)
                {
                    snprintf(buf, sizeof buf, ",\"%s\":%g", t.shortKey, (double)(h.*t.field));
                    j += buf;
                }
            j += "}";
        }
        return j + "}";
    }

    // what the curves see and do right now, for the dashboard's tiles and
    // cards: { "temp": 58.3, "cpu": 37, "gpu": 62,
    //          "header2": { "in": 58.3, "duty": 52 }, ... }
    // — a reading is absent when there is none, "in" when the header has no
    // input (a sensor not found), and a header the receiver runs (fallback,
    // gpio) has no entry at all: the receiver's own view carries those
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
            if (!h.hostRun())
                continue;
            std::string e = "\"" + h.key + "\":{";
            if (h.lastInOk)
                snprintf(buf, sizeof buf, "\"in\":%.1f,", h.lastIn), e += buf;
            snprintf(buf, sizeof buf, "\"duty\":%d}", (int)live_[h.slot]);
            add((e + buf).c_str());
        }
        return j + "}";
    }

    // the catalogue: what a header could follow, with readings, for the
    // phone's source picker — hwmon::enumerate() as JSON grouped by chip:
    //   {"amdgpu":{"edge":61.0,"junction":64.5},
    //    "nct6686":{"CPU":52.0,"System":38.5,"pwm1-8":48}}
    // A chip's pwm outputs are one entry while they have all read alike since
    // the watch began (a board whose firmware drives them from one curve
    // shows one line, named for the range; the spec to follow is its first);
    // the moment two differ they are listed apart for the rest of the watch.
    // WIRE_MAX is a GATT attribute's ceiling, so a machine with more sensors
    // than fit loses entries by priority: pwm outputs first, then labelled
    // temperatures from the end — never a sensor a header follows or the
    // `sensors` pick, which the picker must be able to show as chosen. What
    // was left out is counted in "_more", so the page can say so (the typed
    // field still reaches them). Said once in the journal as well.
    std::string sensorsJson()
    {
        auto all = hwmon::enumerate();

        // the specs in use: every candidate of every header's source and of
        // the top-level sensors pick ("k10temp:Tctl,nct6686:CPU" names two)
        std::vector<std::string> used = hwmon::split(sensors_, ',');
        for (auto& h : headers_)
            for (auto& c : hwmon::split(h.source, ','))
                used.push_back(c);

        // a file: spec in use that the scan of /run/bc250 did not list (its
        // path is the spec's label, and a file spec is its own resolved path)
        for (auto& spec : used)
        {
            if (!hwmon::hasPrefix(spec, hwmon::FILE_PREFIX))
                continue;
            std::string path = spec.substr(strlen(hwmon::FILE_PREFIX));
            bool listed = false;
            for (auto& r : all)
                listed |= r.chip == "file" && r.label == path;
            float v;
            if (!listed && hwmon::readTempOk(spec, v))
                all.push_back({"file", path, false, v});
        }

        struct Entry { std::string chip, key, val; int prio; };
        std::vector<Entry> entries;
        char buf[32];
        auto push = [&](const std::string& chip, const std::string& key, const std::string& spec, float v, bool pwm) {
            if (pwm)
                snprintf(buf, sizeof buf, "%d", (int)(v + 0.5f));
            else
                snprintf(buf, sizeof buf, "%.1f", (double)v);
            bool inUse = std::find(used.begin(), used.end(), spec) != used.end();
            entries.push_back({chip, key, buf, inUse ? 0 : pwm ? 2 : 1});
        };

        std::vector<std::string> chips;
        for (auto& r : all)
            if (std::find(chips.begin(), chips.end(), r.chip) == chips.end())
                chips.push_back(r.chip);
        for (auto& chip : chips)
        {
            std::vector<hwmon::Reading*> pwms;
            for (auto& r : all)
                if (r.chip == chip)
                {
                    if (r.pwm) pwms.push_back(&r);
                    else push(chip, r.label, chip + ":" + r.label, r.value, false);
                }
            if (pwms.empty())
                continue;
            bool& split = pwmSplit_[chip];
            for (auto* p : pwms)
                if ((int)(p->value + 0.5f) != (int)(pwms[0]->value + 0.5f))
                    split = true;
            if (split || pwms.size() == 1)
                for (auto* p : pwms) push(chip, p->label, chip + ":" + p->label, p->value, true);
            else
                push(chip, pwms.front()->label + "-" + pwms.back()->label.substr(3),
                     chip + ":" + pwms.front()->label, pwms[0]->value, true);
        }

        // fit: take entries by priority (order kept within a priority) while
        // the grouped text stays under the ceiling; the size of an entry is
        // its own text plus its chip's wrapper the first time the chip appears
        const size_t RESERVE = sizeof ",\"_more\":999" - 1;
        size_t size = 2; // the braces
        std::vector<bool> take(entries.size(), false);
        std::vector<std::string> open; // chips already counted
        size_t dropped = 0;
        for (int prio = 0; prio <= 2; prio++)
            for (size_t i = 0; i < entries.size(); i++)
            {
                auto& e = entries[i];
                if (e.prio != prio)
                    continue;
                size_t cost = 1 + 1 + cfgedit::escape(e.key).size() + 2 + e.val.size(); // ,"key":val
                if (std::find(open.begin(), open.end(), e.chip) == open.end())
                    cost += 1 + cfgedit::escape(e.chip).size() + 2 + 1 + 1;      // ,"chip":{ ... }
                if (size + cost + RESERVE > (size_t)WIRE_MAX)
                {
                    dropped++;
                    continue;
                }
                size += cost;
                take[i] = true;
                if (std::find(open.begin(), open.end(), e.chip) == open.end())
                    open.push_back(e.chip);
            }

        std::string j = "{";
        for (auto& chip : chips)
        {
            std::string g;
            for (size_t i = 0; i < entries.size(); i++)
                if (take[i] && entries[i].chip == chip)
                    g += (g.empty() ? "" : ",") + std::string("\"") + cfgedit::escape(entries[i].key) + "\":" + entries[i].val;
            if (g.empty())
                continue;
            j += (j.size() > 1 ? "," : "") + std::string("\"") + cfgedit::escape(chip) + "\":{" + g + "}";
        }
        if (dropped)
        {
            j += (j.size() > 1 ? "," : "") + std::string("\"_more\":") + std::to_string(dropped);
            if (!warnedSensors_)
                fprintf(stderr, "fans: the sensor catalogue is over the dashboard's %d bytes — %zu of %zu "
                                "sensors are left out of the phone's picker (they can still be typed)\n",
                        WIRE_MAX, dropped, entries.size());
            warnedSensors_ = true;
        }
        return j + "}";
    }

    void sendSensors(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (now - lastSensorsSent_ < SENSORS_REFRESH_S)
            return;
        lastSensorsSent_ = now;
        std::string j = sensorsJson();
        if (j == lastSensors_)
            return;
        lastSensors_ = j;
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_SENSORS, (const uint8_t*)j.data(), (uint16_t)j.size());
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
    // the config's own parseSource/parseCurve and range checks validate every
    // edit exactly as they validate the file.
    struct HeaderEdit
    {
        Header* h;
        std::string source;
        std::string curve;
        int boost;    // -1 = none
        int fallback;
        std::string name;
        float tuning[TUNING_COUNT];     // TUNINGS' values to run...
        bool tuningGiven[TUNING_COUNT]; // ...and which the edit named (held to the kind)

        HeaderEdit(Header* hp)
            : h(hp), source(hp->source), curve(hp->curveText), boost(hp->boost),
              fallback(hp->fallback), name(hp->name)
        {
            for (int i = 0; i < TUNING_COUNT; i++)
            {
                tuning[i] = hp->*TUNINGS[i].field;
                tuningGiven[i] = false;
            }
        }
    };

    static const size_t NAME_CHARS = 16; // the card's title (characters, not bytes);
                                         // six of these plus curves fit the 512-byte wire

    static size_t utf8Chars(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char c : s)
            n += (c & 0xC0) != 0x80; // count every byte that isn't a continuation
        return n;
    }

    // validate a set of edits and, only if every one passes, apply them.
    // `where` prefixes the error lines. Returns false with nothing changed.
    bool applyValues(const std::vector<HeaderEdit>& edits, const std::string& where)
    {
        // each edit's source and curve, parsed into a scratch copy of its header
        std::vector<Header> next;
        for (auto& e : edits)
        {
            std::string at = where + "." + e.h->key;
            next.push_back(*e.h);
            Header& t = next.back();
            bool moved = e.source != e.h->source;

            if (moved)
            {
                std::string err = parseSource(e.source, sensors_, t);
                if (!err.empty())
                    return bad(at + ".s", err);
                t.source = e.source;
                t.path.clear();
                if (t.kind == Header::Temp || t.kind == Header::Pwm)
                {
                    // a hand in the file may name a sensor that turns up later;
                    // a phone pointing at one that isn't there is told now
                    resolve(t);
                    if (t.path.empty())
                        return bad(at + ".s", "\"" + e.source + "\" is not a sensor on this "
                                              "machine (nothing in /sys/class/hwmon matches)");
                }
            }

            t.curve.clear();
            if (t.kind != Header::Fallback)
            {
                if (e.curve.empty())
                    return bad(at + ".c", "source \"" + t.source + "\" needs a curve");
                if (!parseCurve(e.curve, t.kind, at + ".c", t.curve))
                    return false;
            }
            t.curveText = curveToText(t.curve);

            if (e.boost < -1 || e.boost > 100)
                return bad(at + ".b", "expected a percent 0..100, or null");
            if (e.fallback < 0 || e.fallback > 100)
                return bad(at + ".f", "expected a percent 0..100");
            // only a name the phone changed is held to this — the config may
            // hold any name, and a curve edit echoes it back untouched
            if (e.name != e.h->name && (e.name.empty() || utf8Chars(e.name) > NAME_CHARS))
                return bad(at + ".n", "a name is 1.." + std::to_string(NAME_CHARS) + " characters");
            t.boost = e.boost;
            t.fallback = e.fallback;
            t.name = e.name;
            for (int i = 0; i < TUNING_COUNT; i++)
            {
                const Tuning& tn = TUNINGS[i];
                if (e.tuningGiven[i] && (e.tuning[i] < tn.lo || e.tuning[i] > tn.hi))
                    return bad(at + "." + tn.shortKey, std::string("expected ") + tn.range);
                if (e.tuningGiven[i] && !tn.applies(t))
                    return bad(at + "." + tn.shortKey, std::string(tn.key) + " only applies to " + tn.onlyFor);
                t.*tn.field = e.tuning[i];
            }

            if (moved)
            {
                // a fresh source starts its filters over; the ramp otherwise
                // eases from the old duty to the new curve
                t.haveIn = t.haveOut = false;
                t.reported = false;
            }
        }

        for (size_t i = 0; i < edits.size(); i++)
            *edits[i].h = std::move(next[i]);

        return true;
    }

    // apply a phone's partial edit (the shape in the header comment): every
    // header optional, its s/c/b/f/n/h/r/t each optional (the rest keeps its
    // value), anything else an error. Validated as a whole by applyValues.
    bool applyJson(const json::Value& root, const std::string& where)
    {
        std::vector<HeaderEdit> edits;

        for (auto& m : root.members)
        {
            const std::string& k = m.first;
            const json::Value& v = m.second;
            if (k == "editable")
                continue;
            if (k.rfind("header", 0) == 0 && v.isObject())
            {
                Header* h = nullptr;
                for (auto& o : headers_)
                    if (o.key == k)
                        h = &o;
                if (!h)
                    return bad(where + "." + k, "no such header in the config");

                HeaderEdit e(h);
                for (auto& f : v.members)
                {
                    const std::string& fk = f.first;
                    const json::Value& fv = f.second;
                    int ti = -1;
                    for (int i = 0; i < TUNING_COUNT; i++)
                        if (fk == TUNINGS[i].shortKey)
                            ti = i;
                    if (ti >= 0 && fv.isNumber())
                    {
                        e.tuning[ti] = (float)fv.number;
                        e.tuningGiven[ti] = true;
                    }
                    else if (fk == "s" && fv.isString())
                        e.source = fv.text;
                    else if (fk == "n" && fv.isString())
                        e.name = fv.text;
                    else if (fk == "c" && (fv.isString() || fv.isNumber()))
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

        return applyValues(edits, where);
    }

    // a phone edit: parse, apply, write it into the config, and re-push the
    // standalone part if that moved. False (with a journal line) changes
    // nothing.
    bool applyEdit(const std::string& text, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        const std::string where = "fans (dashboard edit)";

        if (!writable() || headers_.empty())
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
        if (standaloneKey() != before)
            pushStandalone(sinks);
        lastSent_ = -1e9; // send the new duties on the next tick, changed or not

        // the edit runs either way; a failed write is said by the writer and
        // gets no answer, so the phone's timeout names the cause
        return writeConfig();
    }

    // the receiver's standalone settings as pushStandalone sends them: one
    // record per header, a header not in the block left as FAN_NONE
    void standaloneBlob(uint8_t* p) const
    {
        memset(p, proto::FAN_NONE, proto::FAN_STANDALONE_LEN);
        for (auto& h : headers_)
            h.wire().encode(p + h.slot * proto::FAN_HEADER_LEN);
    }

    // ...as one comparable string, for "did an edit move them"
    std::string standaloneKey() const
    {
        uint8_t p[proto::FAN_STANDALONE_LEN];
        standaloneBlob(p);
        return std::string((const char*)p, sizeof p);
    }

    // ---- writing an edit back into the config file ----

    // fold the running values into the block as parsed (every key the user
    // wrote, in their order) and have the writer splice it over the block's
    // bytes in the config file; the rest of the file is untouched. A failure
    // is said and leaves the edit live until the next restart (the writer
    // turns read-only, so the phone stops offering saves that can't stick).
    bool writeConfig()
    {
        for (auto& h : headers_)
        {
            json::Value& hv = cfgedit::member(block_, h.key);
            if (!hv.isObject())
                continue;
            cfgedit::member(hv, "name") = cfgedit::string(h.name);
            cfgedit::member(hv, "source") = cfgedit::string(h.source);
            if (h.kind == Header::Fallback)
                cfgedit::erase(hv, "curve");
            else
            {
                // a curve where the file had none goes after source, where a
                // hand would write it, not at the end
                if (!hv.find("curve"))
                    for (size_t i = 0; i < hv.members.size(); i++)
                        if (hv.members[i].first == "source")
                        {
                            hv.members.insert(hv.members.begin() + i + 1,
                                              std::make_pair(std::string("curve"), json::Value()));
                            break;
                        }
                cfgedit::member(hv, "curve") = cfgedit::string(h.curveText);
            }
            cfgedit::member(hv, "boost") = h.boost < 0 ? json::Value() : cfgedit::number(h.boost);
            cfgedit::member(hv, "fallback") = cfgedit::number(h.fallback);
            // a tuning that stopped applying (the source moved) goes; one at
            // its default is written only where the hand already had it, so
            // a lean file stays lean
            for (auto& t : TUNINGS)
            {
                if (!t.applies(h))
                    cfgedit::erase(hv, t.key);
                else if (hv.find(t.key) || h.*t.field != t.dflt)
                    cfgedit::member(hv, t.key) = cfgedit::number(h.*t.field);
            }
        }

        return writer_->write({BLOCK}, block_, BLOCK);
    }

    std::vector<Header> headers_;

    std::string sensors_;    // the top-level `sensors` spec (telemetry temp)
    json::Value block_;      // the fans block as written (writeConfig updates it)
    cfgedit::Writer* writer_ = nullptr; // the config file's editor; null = read-only

    double watchUntil_ = 0;    // a phone watches until this time (0 = none)
    std::string tempPath_;
    float temp_ = 0;
    bool tempOk_ = false;
    std::string lastTelem_;
    double lastTelemSent_ = -1e9;
    bool warnedSize_ = false;
    std::string lastSensors_;
    double lastSensorsSent_ = -1e9;
    bool warnedSensors_ = false;
    std::map<std::string, bool> pwmSplit_; // chip -> its pwm outputs have differed this watch

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
