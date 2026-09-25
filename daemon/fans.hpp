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
#include "pwmout.hpp"
#include "sink.hpp"

// The daemon's half of the fan controller (README "Fans"). The config's
// "fans" block is a list of fans, each an input read through a curve onto an
// output:
//
//     "fans": [
//         { "name": "pump", "output": "header1", "input": "fallback",
//           "fallback": 65, "boost": 100, "boost_seconds": 5 },
//         { "name": "radiator", "output": "header2", "input": "temp",
//           "curve": "45:35 60:55 75:100", "hysteresis": 3, "ramp": 5, "fallback": 100 },
//         { "name": "board fan", "output": "nct6686:pwm2", "input": "k10temp:Tctl",
//           "curve": "50:30 80:100", "fallback": 60 },
//         ...
//     ]
//
// The list's order is the phone's; nothing else hangs on it. Every fan has a
// name, an output, an input and a fallback, a curve for every input but
// "fallback" (and the board's own, below), optionally a boost, and optionally
// its tunings — hysteresis, ramp, boost_seconds (TUNINGS below: each has a
// default and applies to some fans only; on the others it is refused).
// There is nothing global.
//
// `output` is what the fan drives:
//
//   headerN      header N of the receiver's board (its header → GPIO map is
//                the board's, flash-time: tools/pincheck.py FAN_PINS)
//   gpio:N       a receiver GPIO by number (a hand-wired build)
//   chip:pwmN    a PWM output of THIS host, a hwmon pwmN — the BC-250's own
//                fan header on the NCT6686D, say. This daemon drives it
//                itself (daemon/pwmout.hpp: pwmN_enable taken to manual, and
//                always handed back); the receiver has no part in it
//   ""           nothing: the fan is parked, its settings kept. A receiver
//                header no fan names is not driven at all (a 4-pin fan on it
//                runs full); a host output no fan names runs the board's own
//                curve
//
// Receiver outputs take the receiver's slots (FAN_CHANNELS of them) in list
// order; every one of them moves at runtime — the pin travels in the slot's
// record (protocol.hpp CMD_FAN_STANDALONE).
//
// `input` is what the curve reads, and WHERE the curve runs follows from
// what can read it:
//
//   fallback     nothing: the fan runs its fallback value, always
//   gpio:N       the duty of a PWM signal on the RECEIVER's GPIO N (the
//                BC-250's own fan header, wired over) — the receiver samples
//                it and runs the curve itself, so this works with no daemon
//                and the machine off. x is 0..100 %. A receiver output only
//   temp         the top-level `sensors` pick, °C            (this daemon)
//   chip:label   any hwmon temperature, °C                   (this daemon)
//   pmbus:CPU VRM / pmbus:GPU VRM
//                the BC-250's VRM controller over I2C, °C   (this daemon)
//   smu:VRAM hotspot / average / 0..7
//                the GDDR6 chips over the SMU, °C            (this daemon)
//   file:/path   a file holding one temperature, °C         (this daemon)
//   chip:pwmN    a hwmon pwm output, read as 0..100 %        (this daemon)
//   cpu_load / gpu_load   0..100 %                           (this daemon)
//   ""           a host output only: the board drives it, with its own
//                curve — "output": "nct6686:pwm2", "input": "". This daemon
//                never takes the output, and shows the board's duty; any
//                other input takes it over
//
// A daemon-evaluated ("host") curve on a receiver output goes out as
// CMD_FAN_LIVE whole percents on the rules' 0.5 s tick, sharing the reads the
// rule conditions already do — when something changed, plus a refresh every
// few seconds so the receiver can treat a silence as "the daemon is gone"
// and fall back. On a host output it is written to the chip. An input that
// can't be read (a sensor gone, a thermistor reading 0) runs the fallback,
// on every output alike; a host output goes back to the board's own curve
// only when this daemon stops driving it altogether (it exits, or no fan
// names it).
//
// `curve` is "x:percent" points (up to fancurve::MAX_POINTS), linear between
// and flat beyond the ends; the x unit is the input's. `boost` is the duty
// for the first boost_seconds after the host powers on (null or absent = no
// boost; a receiver output only — the receiver runs it before this daemon
// exists) and `fallback` what the fan runs whenever its input can't be read
// and, on a receiver output, whenever no daemon drives it. Those, the ramp,
// the output, and every receiver fan's input kind (with a gpio fan's pin and
// curve) are the receiver's standalone settings — one fanwire::Header record
// per slot: pushed at startup and after any edit that moves them
// (CMD_FAN_STANDALONE) and also baked into its fancfg partition at flash time
// from this same block (tools/fancfg.py, which assigns the slots the same
// way). The push is the whole truth for every slot — the config is the one
// source of truth while a daemon is connected, and a value dialled on the
// receiver from the phone while no daemon ran is overwritten by it.
//
// The BLE dashboard (README "BLE remote") sees and edits this list through
// the receiver, all of it as JSON text the receiver relays without reading:
// the controller sends the config as it runs it (CMD_FAN_CONFIG, toJson) and,
// while a phone is watching, what the curves read and produce (CMD_FAN_TELEM,
// telemetryJson) and what the machine offers (CMD_FAN_SENSORS, sensorsJson);
// a phone edit comes back as MSG_FAN_CONFIG, is validated exactly like the
// config (every edit is folded into the fan's config object and re-read by
// loadFan) and, once live, is written back into the config file itself: only
// the bytes of the `fans` block are replaced (writeConfig, through
// daemon/config_edit.hpp), everything around it stays as the user wrote it.
// So the config stays the one source of truth — there is no second file to
// migrate — and a restart reads the edit like any other setting.
//
// The JSON shapes the wire and the phone's edits use:
//
//     { "editable": true,        // the config file is writable
//       "rev": "3fa2c019",       // the list's revision, see below
//       "err": "...",            // once, after a refused edit: why
//       "fans": [ { "n": "pump", "o": "header1", "i": "fallback", "b": 100, "t": 5, "f": 65 },
//                 { "n": "radiator", "o": "header2", "i": "temp",
//                   "c": "45:35 60:55 75:100", "f": 100, "h": 3, "r": 5 }, ... ] }
//
// n, o, i are the name, output and input exactly as the config spells them,
// c/b/f the curve, boost and fallback in the config's own notation (c absent
// without a curve, b absent = no boost), h/r/t the tunings (TUNINGS' short
// keys; one is absent when it doesn't apply or sits at its default, which
// the page knows). An edit is ONE operation, naming the revision it was made
// against — a hash of the list as the phone saw it, so an edit made against
// a list that has changed since (another phone, a hand edit, a reload) is
// refused rather than landing on the wrong fan:
//
//     { "rev": "3fa2c019", "fan": 1, "edit": { "c": "40:30 70:100" } }   // some fields of fans[1]
//     { "rev": "3fa2c019", "add": { "n": ..., "o": ..., "i": ..., ... } } // a new fan, at the end
//     { "rev": "3fa2c019", "del": 2 }                                      // drop fans[2]
//
// Keys are short because the phone reads the JSON over GATT, in pages of a
// few hundred bytes (the receiver holds up to DASH_FAN_CONFIG_MAX).
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

struct Fan
{
    std::string name;

    std::string output;              // as written
    enum Out { Parked, Header, GpioOut, Host } out = Parked;
    int outNum = -1;                 // Header: 1-based header number; GpioOut: the GPIO
    std::string outChip, outFile;    // Host: "nct6686", "pwm2"
    int slot = -1;                   // the receiver slot (Header/GpioOut), else -1

    std::string input;               // as written
    enum Kind { Fallback, Gpio, Temp, Pwm, CpuLoad, GpuLoad, Board } kind = Fallback;
    int gpio = -1;                   // Gpio: the receiver's input pin
    std::string spec;                // hwmon candidates (Temp) / chip (Pwm)
    std::string pwmFile;             // "pwm1" (Pwm)
    std::vector<Point> curve;        // sorted by x; empty without one
    std::string curveText;           // the curve, canonical "x:y x:y"
    int boost = -1;                  // percent, -1 = none
    int fallback = 100;
    // the tunings (TUNINGS below); one that doesn't apply to this fan holds
    // its default and is never written out
    float hysteresis = DEFAULT_HYSTERESIS;       // °C a temperature must fall before the fan follows
    float ramp = proto::FAN_DEFAULT_RAMP;        // percent per second on the way down, 0 = at once
    float boostSecs = proto::FAN_DEFAULT_BOOST_SECS;

    // runtime — carried over an edit that leaves the input alone
    struct Runtime
    {
        std::string path;            // the input's resolved file, "" = not yet
        bool lost = false;           // the resolved input stopped reading (said once)
        bool haveIn = false;
        float effIn = 0;             // hysteresis-filtered input
        bool haveOut = false;
        float out = 0;               // ramped output
        float lastIn = 0;            // last raw reading (telemetry, --fan-status)
        bool lastInOk = false;
        int duty = proto::FAN_NONE;  // what this side ran it at this tick (NONE = not ours)
        // a host output
        std::string outPath;         // the resolved pwmN file, "" = not found (yet)
        enum HostSt { HNone, HDrive, HBoard, HRefused, HGone, HReadOnly, HBusy } host = HNone;
        bool saidGone = false, saidRo = false;
    } rt;

    bool receiverOut() const { return out == Header || out == GpioOut; }

    // does this fan's curve read something only this daemon can?
    bool hostInput() const { return kind != Fallback && kind != Gpio && kind != Board; }

    // does it take a curve?
    bool hasCurve() const { return kind != Fallback && kind != Board; }

    float eval(float x) const
    {
        return curve.empty() ? fallback : fancurve::eval(curve.data(), (int)curve.size(), x);
    }

    // the receiver's record of this fan (protocol.hpp CMD_FAN_STANDALONE)
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
        w.outKind = out == Header ? proto::FAN_OUT_HEADER : proto::FAN_OUT_GPIO;
        w.out = (uint8_t)outNum;
        return w;
    }
};

// A fan's tunings: optional numbers with a default, each meaningful for
// some fans only — and refused on the others, so a hysteresis on a load
// input is a typo caught at startup rather than a number that silently does
// nothing. This one table drives the config parser, the dashboard's JSON in
// both directions, the write-back into the file and --fan-status.
struct Tuning
{
    const char* key;      // the config's
    const char* shortKey; // the dashboard's
    const char* range;    // what a value outside lo..hi is told
    float lo, hi;
    float Fan::* field;
    bool (*applies)(const Fan&);
    const char* onlyFor;  // "...only applies to <onlyFor>"
    float dflt;
};
static const Tuning TUNINGS[] = {
    {"hysteresis", "h", "a number of °C, 0 or more", 0, 1e9f, &Fan::hysteresis,
     [](const Fan& f) { return f.kind == Fan::Temp; }, "a temperature input", DEFAULT_HYSTERESIS},
    {"ramp", "r", "percent per second, 0..255 (0 = at once)", 0, 255, &Fan::ramp,
     [](const Fan& f) { return f.hasCurve(); }, "an input with a curve", proto::FAN_DEFAULT_RAMP},
    {"boost_seconds", "t", "seconds, 0..255", 0, 255, &Fan::boostSecs,
     [](const Fan& f) { return f.boost >= 0; }, "a fan with a boost", proto::FAN_DEFAULT_BOOST_SECS},
};
static const int TUNING_COUNT = sizeof TUNINGS / sizeof *TUNINGS;

// the short keys of the other fields, config key → dashboard key (the
// tunings' are in TUNINGS)
struct Field
{
    const char* key;
    const char* shortKey;
};
static const Field FIELDS[] = {
    {"name", "n"}, {"output", "o"}, {"input", "i"}, {"curve", "c"}, {"fallback", "f"}, {"boost", "b"},
};

using hwmon::findChipFile;

// is "label" a pwm output's file name, "pwm1".."pwm99"?
inline bool isPwmLabel(const std::string& label)
{
    return label.size() > 3 && label.compare(0, 3, "pwm") == 0 &&
           label.find_first_not_of("0123456789", 3) == std::string::npos;
}

class Controller
{
public:
    // the top-level config block this module owns. Nothing in it feeds the
    // strip, so a reload confined to it leaves the running effect alone
    // (main.cpp asks each module for its block rather than knowing the names).
    static constexpr const char* BLOCK = "fans";

    // read and validate the "fans" block. Returns false (having said what is
    // wrong, "fans[2].curve: ...") on a bad block, so the daemon can refuse to
    // start the way it does for a bad rule; true with no block, in which case
    // nothing is ever sent. `writer` is the config file's editor
    // (config_edit.hpp), where a dashboard edit is written back (null = edits
    // are refused); `claims` drives the host outputs (pwmout.hpp — null for a
    // look that must never touch one: --fan-status, --check).
    bool load(const Config& cfg, cfgedit::Writer* writer = nullptr, pwmout::Claims* claims = nullptr)
    {
        const json::Value* block = cfg.root().find(BLOCK);

        writer_ = writer;
        claims_ = claims;

        if (!block)
            return true;

        if (block->isObject())
            return retiredShape(*block);

        if (!block->isArray())
            return bad("fans", "expected a list of fans [ { \"name\", \"output\", \"input\", ... }, ... ]");

        sensors_ = cfg.get("sensors", hwmon::DEFAULT_SENSORS);
        stripPin_ = cfg.getInt("strip.pin", 13); // output/strip.hpp's default

        std::vector<Fan> list;
        if (!parseList(*block, list, "fans", true))
            return false;

        fans_ = std::move(list);
        block_ = *block; // the block as written, for writeConfig to update in place
        for (size_t i = 0; i < fans_.size(); i++)
            lastFrom_.push_back((int)i);
        present_ = true;

        for (auto& f : fans_)
            if (f.out == Fan::Host && f.kind != Fan::Board)
                resolveOutput(f);

        return true;
    }

    // is there a fans block at all? (none: the receiver's fans are left alone)
    bool present() const { return present_; }

    bool writable() const { return writer_ && writer_->writable(); }

    // the receiver's standalone settings (protocol.hpp CMD_FAN_STANDALONE) —
    // once, at startup, and again when an edit moves them. Every slot: the
    // config wins over whatever the receiver held (see the header comment)
    void pushStandalone(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (!present_)
            return;

        uint8_t p[proto::FAN_STANDALONE_LEN];
        standaloneBlob(p);
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_STANDALONE, p, sizeof p);
        // the receiver drops a live push still queued behind this one (it
        // may be laid out for the old slots): send the live duties again on
        // the next tick rather than at the next change or REFRESH_S
        lastSent_ = -1e9;
    }

    // one evaluation: read every fan's input this side evaluates, run the
    // curves, drive the host outputs, and send the receiver's live duties if
    // they changed (or the refresh is due). Call it on the rules tick; it
    // costs nothing when there are no fans and no phone is watching (a
    // watcher still gets the tiles' readings).
    void tick(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        bool watch = watching(now);
        if (!present_ && !watch)
        {
            // no fans (a reload dropped the block): anything an earlier config
            // drove is handed back on this, the first tick without it
            if (claims_)
            {
                claims_->begin();
                claims_->end(now);
            }
            return;
        }

        float dt = lastTick_ > 0 ? (float)(now - lastTick_) : 0.5f;
        lastTick_ = now;

        readShared(now);

        uint8_t p[CHANNELS];
        memset(p, proto::FAN_NONE, sizeof p);

        if (claims_)
            claims_->begin();
        for (auto& f : fans_)
        {
            int d = compute(f, dt);
            f.rt.duty = d;
            if (f.slot >= 0)
                p[f.slot] = (uint8_t)d;
            else if (f.out == Fan::Host)
                driveHost(f, d, now);
        }
        if (claims_)
            claims_->end(now);

        bool changed = memcmp(p, live_, sizeof p) != 0;
        bool slots = false;
        for (auto& f : fans_)
            slots |= f.slot >= 0;

        if (changed || (slots && now - lastSent_ >= REFRESH_S))
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
    // and after every applied (or refused) edit
    void pushConfig(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (!present_)
        {
            // no fans block: an empty value, so a list an earlier run left on
            // the receiver isn't shown (and edited, into a daemon that has
            // none) — the phone falls back to the receiver's own slots
            for (auto& s : sinks)
                s->sendCommand(proto::CMD_FAN_CONFIG, nullptr, 0);
            err_.clear();
            return;
        }

        std::string j = toJson();
        // a long refusal on a list near the ceiling: the reason is cut, never
        // the answer (the phone waits for it)
        while (j.size() > proto::DASH_FAN_CONFIG_MAX && !err_.empty())
        {
            size_t cut = err_.size() > 16 ? err_.size() - 16 : 0;
            while (cut > 0 && ((unsigned char)err_[cut] & 0xC0) == 0x80)
                cut--; // not inside a UTF-8 character
            err_.resize(cut);
            j = toJson();
        }
        err_.clear(); // said once
        if (j.size() > proto::DASH_FAN_CONFIG_MAX)
        {
            // the receiver holds DASH_FAN_CONFIG_MAX and would drop this; say
            // so once rather than leave the phone showing nothing
            if (!warnedSize_)
                fprintf(stderr, "fans: the config is %zu bytes as JSON, over the dashboard's "
                                "%u — shorten fan names or inputs\n", j.size(), proto::DASH_FAN_CONFIG_MAX);
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
                lastSensors_.clear();
                pwmSplit_.clear(); // the pwm outputs start out as one entry again
            }
        }
        else if (kind == proto::MSG_FAN_CONFIG)
        {
            // answered either way: the new config, or the old one with why
            applyEdit(std::string(payload.begin(), payload.end()), sinks);
            pushConfig(sinks);
        }
    }

    // ./led <config> --fan-status: what each fan resolves to and would run
    // right now (two ticks, so cpu_load has a delta to report). Drives
    // nothing: the controller was loaded without claims.
    void dumpStatus(FILE* out)
    {
        if (!present_)
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
        fprintf(out, "  sensors a fan could follow (the phone's picker): %s\n", sensorsJson().c_str());
        if (fans_.empty())
            fprintf(out, "  no fans in the list\n");

        for (size_t i = 0; i < fans_.size(); i++)
        {
            Fan& f = fans_[i];
            fprintf(out, "  fans[%zu] %-12s -> %s", i, f.name.c_str(),
                    f.out == Fan::Parked ? "(no output, parked)" : f.output.c_str());
            if (f.slot >= 0)
                fprintf(out, " (receiver slot %d)", f.slot);
            if (f.out == Fan::Host)
            {
                // the board's own is only read, through its input path
                const std::string& p = f.kind == Fan::Board ? f.rt.path : f.rt.outPath;
                fprintf(out, " -> %s", p.empty() ? "(not found)" : p.c_str());
            }
            fprintf(out, "\n      ");

            if (f.kind == Fan::Board)
                fprintf(out, "input: the board's own curve (not taken over)%s",
                        f.rt.lastInOk ? (", at " + std::to_string((int)(f.rt.lastIn + 0.5f)) + "%").c_str() : "");
            else if (f.kind == Fan::Fallback)
                fprintf(out, "input: none — runs its fallback, %d%%%s", f.fallback,
                        f.receiverOut() ? " (the receiver runs it)" : "");
            else if (f.kind == Fan::Gpio)
                fprintf(out, "input: %s \"%s\" (the receiver reads the pin and runs the curve)",
                        f.input.c_str(), f.curveText.c_str());
            else
            {
                fprintf(out, "input: %s", f.input.c_str());
                if (f.kind == Fan::Temp || f.kind == Fan::Pwm)
                    fprintf(out, " -> %s", f.rt.path.empty() ? "(not found)" : f.rt.path.c_str());
                if (f.rt.lastInOk)
                    fprintf(out, " = %g%s", f.rt.lastIn, f.kind == Fan::Temp ? " °C" : " %");
                else
                    fprintf(out, " = (no reading — runs the fallback)");
                if (f.out != Fan::Parked && f.rt.duty != proto::FAN_NONE)
                    fprintf(out, "  -> %d%%", f.rt.duty);
            }

            if (f.out == Fan::Host && f.kind != Fan::Board)
                fprintf(out, " [%s]", hostState(f).text);

            fprintf(out, "\n      fallback %d%%", f.fallback);
            if (f.boost >= 0)
                fprintf(out, ", boost %d%%", f.boost);
            for (auto& t : TUNINGS)
                if (t.applies(f))
                    fprintf(out, ", %s %g", t.key, f.*t.field);
            fprintf(out, "\n");
        }
    }

private:
    bool bad(const std::string& where, const std::string& what)
    {
        fprintf(stderr, "%s: %s\n", where.c_str(), what.c_str());
        if (capture_)
            lastBad_ = what; // the phone's reason (applyEdit)
        return false;
    }

    // the old shape — an object of "header1".."header6" blocks — said as a
    // startup error, with the list it becomes printed ready to paste (there
    // is no compatibility path: one file on one box, and a silently
    // half-read fan block would mean a pump at the wrong speed)
    bool retiredShape(const json::Value& block)
    {
        json::Value list;
        list.type = json::Value::Type::Array;
        std::vector<int> pins;
        if (const json::Value* p = block.find("pins"))
            for (auto& t : hwmon::split(json::toString(*p), ','))
                pins.push_back(atoi(t.c_str()));
        for (int n = 1; n <= CHANNELS; n++)
        {
            const json::Value* h = block.find("header" + std::to_string(n));
            if (!h || !h->isObject())
                continue;
            json::Value f;
            f.type = json::Value::Type::Object;
            std::string out = (int)pins.size() >= n ? "gpio:" + std::to_string(pins[n - 1])
                                                    : "header" + std::to_string(n);
            for (auto& m : h->members)
            {
                if (m.first == "enabled")
                    continue;
                if (m.first == "source")
                {
                    std::string src = json::toString(m.second);
                    cfgedit::member(f, "output") = cfgedit::string(out);
                    cfgedit::member(f, "input") = cfgedit::string(src == "constant" ? "fallback" : src);
                    continue;
                }
                cfgedit::member(f, m.first) = m.second;
            }
            if (!f.find("output"))
                cfgedit::member(f, "output") = cfgedit::string(out);
            list.items.push_back(f);
        }
        std::string text;
        cfgedit::print(list, 1, text);
        fprintf(stderr, "fans: the fans block is a list now, each fan naming its output and its "
                        "input (\"source\" became \"input\"; README \"Fans\") — replace the block "
                        "with:\n    \"fans\": %s\n", text.c_str());
        return false;
    }

    static const char* INPUT_HELP()
    {
        return "expected fallback, gpio:N, temp, cpu_load, gpu_load, a hwmon chip:label / "
               "chip:pwmN, pmbus:CPU VRM / pmbus:GPU VRM, smu:VRAM hotspot / smu:VRAM 0..7, "
               "file:/path, or (for a host output) \"\" for the board's own curve";
    }

    static const char* OUTPUT_HELP()
    {
        return "expected headerN (the receiver's header N), gpio:N (a receiver GPIO), "
               "chip:pwmN (a pwm output of this host, e.g. nct6686:pwm2), or \"\" for none";
    }

    // the N of a "gpio:N", output or input alike; -1 when it isn't a pin number
    // — digits only, no leading zero, so one pin has one spelling (the list
    // check compares outputs as written)
    static int parseGpio(const std::string& n)
    {
        if (n.empty() || n.size() > 3 || n.find_first_not_of("0123456789") != std::string::npos ||
            (n.size() > 1 && n[0] == '0'))
            return -1;
        int v = atoi(n.c_str());
        return v > MAX_GPIO ? -1 : v;
    }
    static std::string GPIO_HELP()
    {
        return "gpio:N names a receiver GPIO, 0.." + std::to_string(MAX_GPIO) +
               " (the flasher and the receiver check it against the chip and the pins other "
               "features use)";
    }

    // what an output string means. "" when it parses, else what is wrong.
    // Only the output fields of f are touched.
    static std::string parseOutput(const std::string& o, Fan& f)
    {
        f.out = Fan::Parked;
        f.outNum = -1;
        f.outChip.clear();
        f.outFile.clear();

        if (o.empty())
            return "";

        if (o.compare(0, 6, "header") == 0)
        {
            const std::string n = o.substr(6);
            if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos ||
                atoi(n.c_str()) < 1 || atoi(n.c_str()) > CHANNELS || n[0] == '0')
                return "headerN names the receiver board's header N, header1..header" +
                       std::to_string(CHANNELS) + " (the board has as many as tools/pincheck.py FAN_PINS lists)";
            f.out = Fan::Header;
            f.outNum = atoi(n.c_str());
            return "";
        }

        size_t colon = o.find(':');
        if (colon == std::string::npos)
            return OUTPUT_HELP();
        std::string chip = o.substr(0, colon), label = o.substr(colon + 1);

        if (chip == "gpio")
        {
            f.outNum = parseGpio(label);
            if (f.outNum < 0)
                return GPIO_HELP();
            f.out = Fan::GpioOut;
            return "";
        }

        if (chip.empty() || chip == "file" || chip == "pmbus" || chip == "smu" || !isPwmLabel(label))
            return OUTPUT_HELP();

        f.out = Fan::Host;
        f.outChip = chip;
        f.outFile = label;
        return "";
    }

    // what an input string means: kind, and for gpio the pin, for hwmon
    // inputs the spec. "" when it parses, else what is wrong with it. Only
    // the input fields of f are touched; the output must be parsed first (the
    // board's own spelling is the output's).
    static std::string parseInput(const std::string& src, const std::string& sensors, Fan& f)
    {
        f.gpio = -1;
        f.spec.clear();
        f.pwmFile.clear();

        if (src.empty())
        {
            if (f.out != Fan::Host)
                return "a blank input is the board's own curve, which only a host output has — "
                       "a fixed speed is input \"fallback\"";
            f.kind = Fan::Board;
            return "";
        }

        if (src == "constant")
            return "renamed: a fixed speed is input \"fallback\" — the fan runs its "
                   "fallback value and takes no curve (move the constant into fallback)";

        if (f.out == Fan::Host && src == f.output)
            return "renamed: the board's own curve is a blank input now — write \"input\": \"\"";

        if (src == "fallback")
        {
            f.kind = Fan::Fallback;
            return "";
        }

        if (src == "temp")
        {
            f.kind = Fan::Temp;
            f.spec = sensors;
            return "";
        }

        if (src == "cpu_load")
        {
            f.kind = Fan::CpuLoad;
            return "";
        }

        if (src == "gpu_load")
        {
            f.kind = Fan::GpuLoad;
            return "";
        }

        size_t colon = src.find(':');
        if (colon == std::string::npos)
            return INPUT_HELP();

        std::string chip = src.substr(0, colon);
        std::string label = src.substr(colon + 1);

        if (chip == "gpio")
        {
            f.gpio = parseGpio(label);
            if (f.gpio < 0)
                return GPIO_HELP();
            f.kind = Fan::Gpio;
            return "";
        }

        // the sources outside hwmon (hwmon.hpp): a spec that could never
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
            if (cchip == "smu" && smu::sourceOf(clabel) < 0)
                return "smu names a GDDR6 reading: smu:VRAM hotspot, smu:VRAM average, "
                       "or smu:VRAM 0..7 (and needs \"vram_temps\": true)";
            if (cchip == "file" && (clabel.empty() || clabel[0] != '/'))
                return "file:/path names a file holding one temperature (millidegrees or degrees)";
        }

        if (isPwmLabel(label))
        {
            f.kind = Fan::Pwm;
            f.spec = chip;
            f.pwmFile = label;
        }
        else
        {
            f.kind = Fan::Temp;
            f.spec = src;
        }
        return "";
    }

    // "45:35 60:55 75:100" → sorted points, for a fan of kind `kind`
    bool parseCurve(const std::string& text, Fan::Kind kind, const std::string& where,
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
                return bad(where, "\"" + t + "\": expected input:percent points, e.g. "
                                  "\"45:35 60:55 75:100\" (a fixed speed is input "
                                  "\"fallback\" with no curve)");
            if (!number(t.substr(0, colon), x) || !number(t.substr(colon + 1), y))
                return bad(where, "\"" + t + "\": not a number pair");
            if (y < 0 || y > 100)
                return bad(where, "\"" + t + "\": percent must be 0..100");
            if (kind == Fan::Gpio && (x < 0 || x > 100 || x != floorf(x)))
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

    // one fan of the list, as the config (or an edit folded into it) spells
    // it. Every problem is said with `where` ("fans[2]").
    bool loadFan(const json::Value& v, const std::string& where, Fan& f)
    {
        if (!v.isObject())
            return bad(where, "expected an object { \"name\", \"output\", \"input\", ... }");

        for (auto& m : v.members)
        {
            if (m.first == "source")
                return bad(where + ".source", "renamed: this is \"input\" now (what the curve "
                                              "reads; \"output\" is what the fan drives)");
            bool known = false;
            for (auto& fl : FIELDS)
                known |= m.first == fl.key;
            for (auto& t : TUNINGS)
                known |= m.first == t.key;
            if (!known)
                return bad(where + "." + m.first, "unknown key");
        }
        for (const char* k : {"name", "output", "input", "fallback"})
            if (!v.find(k))
                return bad(where, std::string("missing \"") + k +
                                      "\" (every fan has a name, an output, an input and a fallback, "
                                      "and a curve unless the input is fallback)");

        const json::Value& name = *v.find("name");
        if (!name.isString())
            return bad(where + ".name", "expected a string");
        f.name = name.text;

        const json::Value& out = *v.find("output");
        if (!out.isString())
            return bad(where + ".output", OUTPUT_HELP());
        f.output = out.text;
        std::string e = parseOutput(f.output, f);
        if (!e.empty())
            return bad(where + ".output", e);

        const json::Value& in = *v.find("input");
        if (!in.isString())
            return bad(where + ".input", INPUT_HELP());
        f.input = in.text;
        e = parseInput(f.input, sensors_, f);
        if (!e.empty())
            return bad(where + ".input", e);

        if (f.kind == Fan::Gpio && f.out == Fan::Host)
            return bad(where + ".input", "a gpio input is read by the receiver, which can't drive a "
                                         "host output — put the fan on a receiver output (headerN / "
                                         "gpio:N), or give it a host input");
        if (f.kind == Fan::Gpio && f.out == Fan::GpioOut && f.gpio == f.outNum)
            return bad(where + ".input", "GPIO" + std::to_string(f.gpio) + " can't be the fan's "
                                         "input and its output at once");

        const json::Value* curve = v.find("curve");
        f.curve.clear();
        f.curveText.clear();
        if (!f.hasCurve())
        {
            if (curve)
                return bad(where + ".curve", f.kind == Fan::Board
                                                 ? "the board's own curve runs this output — delete this key"
                                                 : "a fallback input takes no curve — the fan runs its "
                                                   "fallback value; delete this key");
        }
        else
        {
            if (!curve)
                return bad(where, "missing \"curve\" (input:percent points, e.g. "
                                  "\"45:35 60:55 75:100\")");
            if (!curve->isString())
                return bad(where + ".curve", "expected a string like \"45:35 60:55 75:100\"");
            if (!parseCurve(curve->text, f.kind, where + ".curve", f.curve))
                return false;
            f.curveText = curveToText(f.curve);
        }

        f.boost = -1;
        if (const json::Value* boost = v.find("boost"))
        {
            if (boost->type == json::Value::Type::Null)
                f.boost = -1;
            else if (boost->isNumber() && boost->number >= 0 && boost->number <= 100)
                f.boost = (int)(boost->number + 0.5);
            else
                return bad(where + ".boost", "expected a percent 0..100, or null for no boost");
            if (f.boost >= 0 && f.out == Fan::Host)
                return bad(where + ".boost", "a boost is the receiver's, run the moment the host "
                                             "powers on — before this daemon exists to drive a host "
                                             "output; write null");
        }

        const json::Value& fb = *v.find("fallback");
        if (!fb.isNumber() || fb.number < 0 || fb.number > 100)
            return bad(where + ".fallback", "expected a percent 0..100");
        f.fallback = (int)(fb.number + 0.5);

        for (auto& t : TUNINGS)
        {
            f.*t.field = t.dflt;
            const json::Value* tv = v.find(t.key);
            if (!tv)
                continue;
            if (!tv->isNumber() || tv->number < t.lo || tv->number > t.hi)
                return bad(where + "." + t.key, std::string("expected ") + t.range);
            if (!t.applies(f))
                return bad(where + "." + t.key, std::string(t.key) + " only applies to " + t.onlyFor +
                                                    " — delete this key");
            f.*t.field = (float)tv->number;
        }

        return true;
    }

    // the whole list: each fan, then what only the list can say — an output
    // given twice, more receiver outputs than the receiver has, a pin read by
    // one fan and driven by another — and the receiver slots, in list order.
    // `startup` says the not-found-yet inputs (a hand in the file may name a
    // sensor that turns up later); an edit is held to more (applyList).
    bool parseList(const json::Value& arr, std::vector<Fan>& out, const std::string& where, bool startup)
    {
        out.clear();
        for (size_t i = 0; i < arr.items.size(); i++)
        {
            Fan f;
            if (!loadFan(arr.items[i], where + "[" + std::to_string(i) + "]", f))
                return false;
            out.push_back(std::move(f));
        }

        int slot = 0;
        for (size_t i = 0; i < out.size(); i++)
        {
            Fan& f = out[i];
            std::string at = where + "[" + std::to_string(i) + "]";
            for (size_t j = 0; j < i; j++)
                if (!f.output.empty() && out[j].output == f.output)
                    return bad(at + ".output", "\"" + f.output + "\" is fans[" + std::to_string(j) +
                                                   "]'s output already — one fan per output");
            if (f.out == Fan::GpioOut)
                for (size_t j = 0; j < out.size(); j++)
                    if (out[j].kind == Fan::Gpio && out[j].gpio == f.outNum)
                        return bad(at + ".output", "GPIO" + std::to_string(f.outNum) + " is fans[" +
                                                       std::to_string(j) + "]'s gpio input — an output "
                                                       "needs a pin of its own");
            // a pwm input reads what the board asks of that output; one this
            // daemon drives for another fan would read our own writes back
            if (f.kind == Fan::Pwm)
                for (size_t j = 0; j < out.size(); j++)
                    if (out[j].out == Fan::Host && out[j].kind != Fan::Board && out[j].outChip == f.spec &&
                        out[j].outFile == f.pwmFile)
                        return bad(at + ".input", "\"" + f.input + "\" is fans[" + std::to_string(j) +
                                                      "]'s output — this daemon drives it, so it would "
                                                      "read its own duty back, not the board's");
            // a header's pin is the receiver's to know (it refuses the strip's
            // too); a pin by number is caught here, before it reaches it
            if (f.out == Fan::GpioOut && f.outNum == stripPin_)
                return bad(at + ".output", "GPIO" + std::to_string(f.outNum) + " is the strip's data pin "
                                               "(strip.pin)");
            if (f.kind == Fan::Gpio && f.gpio == stripPin_)
                return bad(at + ".input", "GPIO" + std::to_string(f.gpio) + " is the strip's data pin "
                                              "(strip.pin)");
            f.slot = -1;
            if (f.receiverOut())
            {
                if (slot >= CHANNELS)
                    return bad(at + ".output", "more than " + std::to_string(CHANNELS) + " fans on "
                                                   "receiver outputs — it has " + std::to_string(CHANNELS) +
                                                   " PWM channels");
                f.slot = slot++;
            }
        }

        if (startup)
            for (size_t i = 0; i < out.size(); i++)
            {
                Fan& f = out[i];
                if (f.kind == Fan::Temp || f.kind == Fan::Pwm)
                {
                    resolve(f);
                    if (f.rt.path.empty())
                        fprintf(stderr, "%s[%zu] (%s): %s not found yet, will keep looking\n",
                                where.c_str(), i, f.name.c_str(), f.input.c_str());
                }
            }

        return true;
    }

    void resolve(Fan& f)
    {
        if (f.kind == Fan::Temp)
            f.rt.path = hwmon::findSensorFromSpec(f.spec);
        else if (f.kind == Fan::Pwm)
            f.rt.path = findChipFile(f.spec, f.pwmFile);
        else if (f.kind == Fan::Board)
            f.rt.path = findChipFile(f.outChip, f.outFile);
    }

    // find a host output's pwmN file, saying once when there is none (yet:
    // at boot the hwmon driver may still be loading)
    void resolveOutput(Fan& f)
    {
        f.rt.outPath = findChipFile(f.outChip, f.outFile);
        if (f.rt.outPath.empty() && !f.rt.saidGone)
        {
            f.rt.saidGone = true;
            fprintf(stderr, "fans: %s (%s): no such pwm output on this machine yet, will keep looking\n",
                    f.output.c_str(), f.name.c_str());
        }
    }

    // the readings more than one fan may want, once per tick. A watching
    // phone wants all of them (the dashboard's tiles); otherwise only what
    // some curve reads is read at all.
    void readShared(double now)
    {
        bool watch = watching(now);
        bool wantCpu = watch, wantGpu = watch;
        for (auto& f : fans_)
        {
            if (f.out == Fan::Parked)
                continue;
            wantCpu |= f.kind == Fan::CpuLoad;
            wantGpu |= f.kind == Fan::GpuLoad;
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

    bool readInput(Fan& f, float& v, size_t idx)
    {
        switch (f.kind)
        {
            case Fan::Fallback:
            case Fan::Gpio:
                return false; // nothing this side reads

            case Fan::CpuLoad:
                v = cpuLoad_;
                return cpuLoadOk_;

            case Fan::GpuLoad:
                v = gpuLoad_;
                return gpuLoadOk_;

            case Fan::Temp:
            case Fan::Pwm:
            case Fan::Board:
                if (f.rt.path.empty())
                {
                    resolve(f);
                    if (f.rt.path.empty())
                        return false;
                    fprintf(stderr, "fans[%zu] (%s): using %s\n", idx, f.name.c_str(), f.rt.path.c_str());
                }

                if (f.kind == Fan::Temp)
                {
                    // per tick, one read per path; NaN = no usable reading
                    auto it = temps_.find(f.rt.path);
                    if (it == temps_.end())
                    {
                        float t;
                        it = temps_.emplace(f.rt.path, hwmon::readTempOk(f.rt.path, t) ? t : NAN).first;
                    }
                    if (std::isnan(it->second))
                    {
                        // an unplugged thermistor reads 0, a gone chip reads
                        // nothing: either way the fan runs its fallback
                        // rather than a curve fed a temperature nobody measured
                        if (!f.rt.lost)
                            fprintf(stderr, "fans[%zu] (%s): %s gives no usable reading — running the "
                                            "fallback until it does\n", idx, f.name.c_str(), f.rt.path.c_str());
                        f.rt.lost = true;
                        if (!hwmon::fileExists(f.rt.path))
                            f.rt.path.clear(); // gone: look it up again — it may return under another hwmon number
                        return false;
                    }
                    if (f.rt.lost)
                        fprintf(stderr, "fans[%zu] (%s): %s is reading again\n", idx, f.name.c_str(), f.rt.path.c_str());
                    f.rt.lost = false;
                    v = it->second;
                    return true;
                }

                {
                    int raw;
                    if (!pwmout::readInt(f.rt.path, raw))
                    {
                        if (!hwmon::fileExists(f.rt.path))
                            f.rt.path.clear();
                        return false;
                    }
                    v = raw * 100.0f / 255.0f; // 0..255
                    if (v < 0) v = 0;
                    if (v > 100) v = 100;
                    return true;
                }
        }

        return false;
    }

    // one fan's duty this tick, as this side runs it: read, hysteresis
    // (temperatures), curve, ramp. An input that can't be read runs the
    // fallback; a fan this side doesn't run is FAN_NONE — parked, the
    // receiver's own (a fallback or gpio fan on a receiver output), or the
    // board's (its reading is kept for the dashboard).
    int compute(Fan& f, float dt)
    {
        const size_t idx = &f - fans_.data();
        if (f.out == Fan::Parked)
        {
            f.rt.lastInOk = false;
            return proto::FAN_NONE;
        }

        if (f.kind == Fan::Board)
        {
            float in;
            f.rt.lastInOk = readInput(f, in, idx);
            if (f.rt.lastInOk)
                f.rt.lastIn = in;
            return proto::FAN_NONE;
        }

        if (f.kind == Fan::Fallback)
            return f.receiverOut() ? proto::FAN_NONE : f.fallback;

        if (f.kind == Fan::Gpio)
            return proto::FAN_NONE;

        float in;
        f.rt.lastInOk = readInput(f, in, idx);
        if (!f.rt.lastInOk)
        {
            f.rt.haveIn = f.rt.haveOut = false;
            return f.fallback;
        }
        f.rt.lastIn = in;

        // hysteresis: a temperature has to fall the fan's `hysteresis`
        // below the value the fan is running for before the fan follows it
        // down; rises are taken at once. Percent inputs (loads, a mirrored
        // pwm) skip it.
        if (f.kind == Fan::Temp)
        {
            if (!f.rt.haveIn || in > f.rt.effIn || in < f.rt.effIn - f.hysteresis)
                f.rt.effIn = in;
        }
        else
            f.rt.effIn = in;
        f.rt.haveIn = true;

        f.rt.out = fancurve::ramp(f.eval(f.rt.effIn), f.rt.out, f.rt.haveOut, f.ramp, dt);
        f.rt.haveOut = true;

        return (int)(f.rt.out + 0.5f);
    }

    // a host output's turn: find its pwmN, then have the claims drive it at
    // `duty` — or not, and say why on the dashboard. The board's own (a blank
    // input) is never driven: not naming it is what hands it back.
    void driveHost(Fan& f, int duty, double now)
    {
        if (f.kind == Fan::Board)
        {
            f.rt.host = Fan::Runtime::HBoard;
            return;
        }

        // the path found once must still be there AND still be the named
        // chip's: a driver reloaded mid-run hands its hwmonN to whichever
        // chip registers next, and that chip may have a pwmN too — driving
        // (and re-asserting manual on) it would take over a fan nobody named
        // and whose setting the claim never recorded. One small sysfs read a
        // tick; the claims follow the output by name to where it is now.
        if (!f.rt.outPath.empty())
        {
            bool gone = !hwmon::statExists(f.rt.outPath);
            if (gone || hwmon::chipOfFile(f.rt.outPath) != f.outChip)
            {
                fprintf(stderr, "fans: %s (%s): %s %s — looking for it again\n", f.output.c_str(),
                        f.name.c_str(), f.rt.outPath.c_str(), gone ? "is gone" : "is another chip's now");
                f.rt.outPath.clear();
                f.rt.saidGone = false;
            }
        }
        if (f.rt.outPath.empty())
            resolveOutput(f);
        if (f.rt.outPath.empty())
        {
            f.rt.host = Fan::Runtime::HGone;
            return;
        }
        if (f.rt.saidGone)
        {
            fprintf(stderr, "fans: %s (%s): found %s\n", f.output.c_str(), f.name.c_str(), f.rt.outPath.c_str());
            f.rt.saidGone = false;
        }

        if (!pwmout::writable(f.rt.outPath))
        {
            if (!f.rt.saidRo)
                fprintf(stderr, "fans: %s (%s): %s can be read but not set — this driver is read-only "
                                "(on the BC-250 the in-kernel nct6683 is; the nct6687 driver can set it), "
                                "so the board's own curve keeps running it\n",
                        f.output.c_str(), f.name.c_str(), f.rt.outPath.c_str());
            f.rt.saidRo = true;
            f.rt.host = Fan::Runtime::HReadOnly;
            return;
        }
        f.rt.saidRo = false;

        // the claims open on the first host output that needs them (and a
        // lock busy then is retried now and then)
        if (!claims_ || !claims_->want(now))
        {
            f.rt.host = Fan::Runtime::HBusy;
            return;
        }

        f.rt.host = claims_->drive(f.rt.outPath, f.output, duty, now) ? Fan::Runtime::HDrive
                                                                     : Fan::Runtime::HRefused;
    }

    // a host output's state, one row per Runtime::HostSt: the phone's code
    // (telemetry "st"; null = none sent) and the --fan-status text
    struct HostStateRow
    {
        Fan::Runtime::HostSt st;
        const char* code;
        const char* text;
    };
    static constexpr HostStateRow HOST_STATES[] = {
        {Fan::Runtime::HNone, nullptr, ""},
        {Fan::Runtime::HDrive, "host", "driven by this daemon"},
        {Fan::Runtime::HBoard, "board", "the board's own curve"},
        // the phone shows both as the board's; the journal says why this one is
        {Fan::Runtime::HRefused, "board", "the board's own curve (the takeover failed — see the journal)"},
        {Fan::Runtime::HGone, "gone", "no such output on this machine (yet)"},
        {Fan::Runtime::HReadOnly, "ro", "read-only driver: the board's own curve"},
        {Fan::Runtime::HBusy, "busy", "not driven here (a look only, or another daemon drives it)"},
    };
    static const HostStateRow& hostState(const Fan& f)
    {
        for (auto& r : HOST_STATES)
            if (r.st == f.rt.host)
                return r;
        return HOST_STATES[0];
    }

    // ---- dashboard helpers ----

    static const size_t ERR_ROOM = 160; // the longest "err" an answer adds to toJson
    static const int WATCH_S = 30; // a MSG_FAN_WATCH 1 keeps telemetry flowing this long
    static const int TELEM_REFRESH_S = 5;
    static const int SENSORS_REFRESH_S = 5; // the catalogue's pace while watched

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

    // one fan in the dashboard's shape (see the header comment)
    static std::string fanJson(const Fan& f)
    {
        char buf[40];
        std::string j = "{\"n\":\"" + cfgedit::escape(f.name) + "\",\"o\":\"" + cfgedit::escape(f.output) +
                        "\",\"i\":\"" + cfgedit::escape(f.input) + "\"";
        if (f.hasCurve())
            j += ",\"c\":\"" + f.curveText + "\"";
        if (f.boost >= 0)
            j += ",\"b\":" + std::to_string(f.boost);
        j += ",\"f\":" + std::to_string(f.fallback);
        // a tuning travels when it applies and moved off its default; the
        // page fills in the defaults (the wire's, protocol.hpp)
        for (auto& t : TUNINGS)
            if (t.applies(f) && f.*t.field != t.dflt)
            {
                snprintf(buf, sizeof buf, ",\"%s\":%g", t.shortKey, (double)(f.*t.field));
                j += buf;
            }
        return j + "}";
    }

    std::string listJson() const
    {
        std::string j = "[";
        for (size_t i = 0; i < fans_.size(); i++)
            j += (i ? "," : "") + fanJson(fans_[i]);
        return j + "]";
    }

    // the list's revision: FNV-1a over its JSON — what the phone saw is what
    // an edit must name, whoever changed the list since
    std::string rev() const
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : listJson())
        {
            h ^= c;
            h *= 16777619u;
        }
        char buf[12];
        snprintf(buf, sizeof buf, "%08x", h);
        return buf;
    }

    // the list as run, for the dashboard (see the header comment)
    std::string toJson() const
    {
        std::string j = std::string("{\"editable\":") + (writable() ? "true" : "false") +
                        ",\"rev\":\"" + rev() + "\"";
        if (!err_.empty())
            j += ",\"err\":\"" + cfgedit::escape(err_) + "\"";
        return j + ",\"fans\":" + listJson() + "}";
    }

    // what the curves see and do right now, for the dashboard's tiles and
    // cards: { "temp": 58.3, "cpu": 37, "gpu": 62,
    //          "fans": [ null, { "in": 58.3, "duty": 52 }, { "duty": 60, "st": "host" }, ... ] }
    // — indexed like the config's list. A reading is absent when there is
    // none; a fan the receiver runs (a fallback or gpio fan on a receiver
    // output: its own view carries those) and a parked fan are null. "st" is
    // a host output's state: host (this daemon drives it), board (its own
    // curve), gone (not found), ro (read-only driver), busy (not driven here)
    std::string telemetryJson() const
    {
        char buf[64];
        std::string j = "{";
        auto add = [&](const std::string& s) { if (j.size() > 1) j += ','; j += s; };
        if (tempOk_)
            snprintf(buf, sizeof buf, "\"temp\":%.1f", temp_), add(buf);
        if (cpuLoadOk_)
            snprintf(buf, sizeof buf, "\"cpu\":%d", (int)(cpuLoad_ + 0.5f)), add(buf);
        if (gpuLoadOk_)
            snprintf(buf, sizeof buf, "\"gpu\":%d", (int)(gpuLoad_ + 0.5f)), add(buf);
        std::string list = "\"fans\":[";
        for (size_t i = 0; i < fans_.size(); i++)
        {
            const Fan& f = fans_[i];
            if (i)
                list += ",";
            bool receiverRun = f.receiverOut() && !f.hostInput();
            if (f.out == Fan::Parked || receiverRun)
            {
                list += "null";
                continue;
            }
            std::string e = "{";
            auto field = [&](const char* s) { if (e.size() > 1) e += ','; e += s; };
            if (f.rt.lastInOk)
                snprintf(buf, sizeof buf, "\"in\":%.1f", f.rt.lastIn), field(buf);
            int duty = f.kind == Fan::Board ? (f.rt.lastInOk ? (int)(f.rt.lastIn + 0.5f) : -1)
                     : f.out == Fan::Host && f.rt.host != Fan::Runtime::HDrive ? -1
                     : f.rt.duty == proto::FAN_NONE ? -1 : f.rt.duty;
            if (duty >= 0)
                snprintf(buf, sizeof buf, "\"duty\":%d", duty), field(buf);
            if (f.out == Fan::Host)
                if (const char* st = hostState(f).code)
                    snprintf(buf, sizeof buf, "\"st\":\"%s\"", st), field(buf);
            list += e + "}";
        }
        add(list + "]");
        return j + "}";
    }

    // the catalogue: what a fan could follow and drive, with readings, for
    // the phone's pickers — hwmon::enumerate() as JSON grouped by chip, and
    // every host pwm output (from the same scan) under "_outs":
    //   {"amdgpu":{"edge":61.0,"junction":64.5},
    //    "nct6686":{"CPU":52.0,"System":38.5,"pwm1-8":48},
    //    "_outs":[["nct6686:pwm1",48,1450,1],["nct6686:pwm2",48,-1,1],...]}
    // an output is [spec, duty %, rpm of the same-numbered tachometer (-1 =
    // none), 1 = can be driven]. A chip's pwm outputs are one input entry
    // while they have all read alike since the watch began (a board whose
    // firmware drives them from one curve shows one line, named for the
    // range; the spec to follow is its first); the moment two differ they
    // are listed apart for the rest of the watch. DASH_FAN_SENSORS_MAX is the
    // receiver's buffer, so a machine with more sensors than fit loses input
    // entries by priority: pwm outputs first, then labelled temperatures from
    // the end — never an input or output a fan names or the `sensors` pick,
    // which the pickers must be able to show as chosen. "_outs" is held to
    // half the ceiling the same way (the outputs a fan names always kept).
    // What was left out is counted in "_more", so the page can say so (the
    // typed field still reaches them). Said once in the journal as well.
    std::string sensorsJson()
    {
        auto all = hwmon::enumerate();

        // the specs in use: every candidate of every fan's input and of the
        // top-level sensors pick ("k10temp:Tctl,nct6686:CPU" names two)
        std::vector<std::string> used = hwmon::split(sensors_, ',');
        for (auto& f : fans_)
        {
            for (auto& c : hwmon::split(f.input, ','))
                used.push_back(c);
            if (f.out == Fan::Host)
                used.push_back(f.output);
        }

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
                all.push_back({"file", path, false, v, ""});
        }

        // the host's pwm outputs, from the same scan: the ones a fan names
        // always, the rest while they take at most half the ceiling (the
        // input entries need the other half)
        std::string outs = "\"_outs\":[";
        bool firstOut = true;
        size_t outsLeft = 0;
        for (int pass = 0; pass < 2; pass++)
            for (auto& r : all)
            {
                if (!r.pwm)
                    continue;
                const std::string spec = r.chip + ":" + r.label;
                bool inUse = std::find(used.begin(), used.end(), spec) != used.end();
                if (inUse != (pass == 0))
                    continue;
                int rpm = -1;
                if (!pwmout::readInt(r.path.substr(0, r.path.rfind('/')) + "/fan" + r.label.substr(3) + "_input", rpm) ||
                    rpm < 0)
                    rpm = -1;
                std::string e = std::string(firstOut ? "" : ",") + "[\"" + cfgedit::escape(spec) + "\"," +
                                std::to_string((int)(r.value + 0.5f)) + "," + std::to_string(rpm) + "," +
                                (pwmout::writable(r.path) ? "1" : "0") + "]";
                if (!inUse && outs.size() + e.size() > proto::DASH_FAN_SENSORS_MAX / 2)
                {
                    outsLeft++;
                    continue;
                }
                outs += e;
                firstOut = false;
            }
        outs += "]";
        if (outsLeft && !warnedOuts_)
        {
            fprintf(stderr, "fans: %zu host pwm outputs left out of the phone's output picker (the "
                            "catalogue is over the dashboard's %u bytes; they can still be typed)\n",
                    outsLeft, proto::DASH_FAN_SENSORS_MAX);
            warnedOuts_ = true;
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
        const size_t RESERVE = sizeof ",\"_more\":999" - 1 + 1 + outs.size();
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
                if (size + cost + RESERVE > (size_t)proto::DASH_FAN_SENSORS_MAX)
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
                fprintf(stderr, "fans: the sensor catalogue is over the dashboard's %u bytes — %zu of %zu "
                                "sensors are left out of the phone's picker (they can still be typed)\n",
                        proto::DASH_FAN_SENSORS_MAX, dropped, entries.size());
            warnedSensors_ = true;
        }
        j += (j.size() > 1 ? "," : "") + outs;
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
        if (j.size() > proto::DASH_FAN_SENSORS_MAX)
        {
            // only what the fans name is over the ceiling (the rest is trimmed
            // to fit): the receiver would drop it, so say so instead
            if (!warnedSensorsSize_)
                fprintf(stderr, "fans: the sensor catalogue is %zu bytes even trimmed, over the "
                                "dashboard's %u — not sent (shorten the fans' inputs)\n",
                        j.size(), proto::DASH_FAN_SENSORS_MAX);
            warnedSensorsSize_ = true;
            return;
        }
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_SENSORS, (const uint8_t*)j.data(), (uint16_t)j.size());
    }

    void sendTelemetry(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        std::string j = telemetryJson();
        if (j == lastTelem_ && now - lastTelemSent_ < TELEM_REFRESH_S)
            return;
        if (j.size() > proto::DASH_FAN_TELEM_MAX)
        {
            if (!warnedTelem_)
                fprintf(stderr, "fans: the telemetry is %zu bytes, over the dashboard's %u — not sent\n",
                        j.size(), proto::DASH_FAN_TELEM_MAX);
            warnedTelem_ = true;
            return;
        }

        lastTelem_ = j;
        lastTelemSent_ = now;
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_FAN_TELEM, (const uint8_t*)j.data(), (uint16_t)j.size());
    }

    // ---- edits ----

    static const size_t NAME_CHARS = 16; // the card's title (characters, not bytes)

    static size_t utf8Chars(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char c : s)
            n += (c & 0xC0) != 0x80; // count every byte that isn't a continuation
        return n;
    }

    // a fan as the config would spell it, from the running values: what an
    // edit is folded into, so loadFan validates the result exactly as it
    // validates the file
    static json::Value configObject(const Fan& f)
    {
        json::Value o;
        o.type = json::Value::Type::Object;
        cfgedit::member(o, "name") = cfgedit::string(f.name);
        cfgedit::member(o, "output") = cfgedit::string(f.output);
        cfgedit::member(o, "input") = cfgedit::string(f.input);
        if (f.hasCurve())
            cfgedit::member(o, "curve") = cfgedit::string(f.curveText);
        cfgedit::member(o, "fallback") = cfgedit::number(f.fallback);
        if (f.boost >= 0)
            cfgedit::member(o, "boost") = cfgedit::number(f.boost);
        for (auto& t : TUNINGS)
            if (t.applies(f) && f.*t.field != t.dflt)
                cfgedit::member(o, t.key) = cfgedit::number(f.*t.field);
        return o;
    }

    // a dashboard object (short keys) as config keys, into `into`; the keys
    // it named go into `named`. False on a key or value it can't take.
    bool fromShort(const json::Value& e, json::Value& into, std::vector<std::string>& named,
                   const std::string& where)
    {
        if (!e.isObject())
            return bad(where, "expected an object");
        for (auto& m : e.members)
        {
            const char* key = nullptr;
            for (auto& fl : FIELDS)
                if (m.first == fl.shortKey)
                    key = fl.key;
            for (auto& t : TUNINGS)
                if (m.first == t.shortKey)
                    key = t.key;
            if (!key)
                return bad(where + "." + m.first, "not understood");
            // a curve typed as a single number is still a curve for loadFan
            // to refuse, not a crash
            cfgedit::member(into, key) = m.second;
            named.push_back(key);
        }
        return true;
    }

    // fold a partial edit into a fan's config object. A key the edit didn't
    // name that stops applying once it lands — the curve of a fan whose input
    // became fallback, a hysteresis once the input isn't a temperature, a
    // boost once the output is the host's — goes quietly, as the write-back
    // drops it from the file; a key the edit did name is held to loadFan.
    void fold(json::Value& obj, const std::vector<std::string>& named)
    {
        auto has = [&](const char* k) { return std::find(named.begin(), named.end(), k) != named.end(); };
        Fan scratch;
        const json::Value* o = obj.find("output");
        const json::Value* i = obj.find("input");
        if (!o || !i || !o->isString() || !i->isString() || !parseOutput(o->text, scratch).empty())
            return;
        scratch.output = o->text;
        if (!parseInput(i->text, sensors_, scratch).empty())
            return;
        if (!has("curve") && !scratch.hasCurve())
            cfgedit::erase(obj, "curve");
        if (!has("boost") && scratch.out == Fan::Host)
            cfgedit::erase(obj, "boost");
        const json::Value* b = obj.find("boost");
        scratch.boost = b && b->isNumber() ? (int)b->number : -1;
        for (auto& t : TUNINGS)
            if (!has(t.key) && !t.applies(scratch))
                cfgedit::erase(obj, t.key);
    }

    // validate a whole new list (the edit applied to the running one's config
    // objects) and, if it passes, make it the running list. `from` is the old
    // index each new fan continues (-1: new), so the runtime state (filters,
    // ramp, a resolved sensor) carries over where the fan's input is the same,
    // and the write-back finds the file's object each fan came from. Returns
    // false with nothing changed.
    bool applyList(const json::Value& arr, const std::vector<int>& from, const std::string& where)
    {
        std::vector<Fan> next;
        if (!parseList(arr, next, where, false))
            return false;

        for (size_t i = 0; i < next.size(); i++)
        {
            Fan& n = next[i];
            const Fan* old = from[i] >= 0 ? &fans_[from[i]] : nullptr;
            const std::string at = where + "[" + std::to_string(i) + "]";

            // a name the phone changed is held to the card's width — the
            // config may hold any name, and an edit elsewhere leaves it be
            if ((!old || old->name != n.name) && (n.name.empty() || utf8Chars(n.name) > NAME_CHARS))
                return bad(at + ".n", "a name is 1.." + std::to_string(NAME_CHARS) + " characters");

            // what the input reads depends on the output too (the board's own
            // curve, a blank input, reads the output)
            bool inputMoved = !old || old->input != n.input || old->output != n.output;
            bool outputMoved = !old || old->output != n.output;

            // a phone pointing at a sensor or an output that isn't there is
            // told now (a hand in the file may name one that turns up later)
            if (inputMoved && (n.kind == Fan::Temp || n.kind == Fan::Pwm))
            {
                resolve(n);
                if (n.rt.path.empty())
                    return bad(at + ".i", "\"" + n.input + "\" is not a sensor on this machine "
                                          "(nothing in " + hwmon::root() + " matches)");
            }
            // an output handed from the board to the host is checked like a
            // new one: taking it over is what needs a writable driver
            bool takenOver = old && old->kind == Fan::Board && n.kind != Fan::Board;
            if ((outputMoved || takenOver) && n.out == Fan::Host)
            {
                std::string p = findChipFile(n.outChip, n.outFile);
                if (p.empty())
                    return bad(at + ".o", "\"" + n.output + "\" is not a pwm output on this machine");
                if (n.kind != Fan::Board && !pwmout::writable(p))
                    return bad(at + ".o", "\"" + n.output + "\" is read-only here — its driver can't set "
                                          "it (on the BC-250 the nct6687 driver can, the in-kernel nct6683 "
                                          "can't)");
            }

            if (old)
            {
                // the same fan: keep what it has learnt. A fresh input starts
                // its filters over (the ramp otherwise eases from the old duty
                // to the new curve); a fresh output is looked up again
                Fan::Runtime rt = old->rt;
                if (inputMoved)
                {
                    rt.path = n.rt.path; // just resolved, or "" (looked up on the tick)
                    rt.lost = false;
                    rt.haveIn = rt.haveOut = false;
                    rt.lastInOk = false;
                }
                if (outputMoved)
                {
                    rt.outPath.clear();
                    rt.host = Fan::Runtime::HNone;
                    rt.saidGone = rt.saidRo = false;
                }
                n.rt = rt;
            }
            if (n.out == Fan::Host && n.kind != Fan::Board && n.rt.outPath.empty())
                resolveOutput(n);
        }

        // the list must still reach the phone: past the receiver's ceiling
        // the answer to this edit (and every later one) would never arrive,
        // leaving the phone on a revision the daemon no longer has. Room is
        // kept for the "err" an answer may carry
        std::vector<Fan> prev = std::move(fans_);
        fans_ = std::move(next);
        if (toJson().size() + ERR_ROOM > proto::DASH_FAN_CONFIG_MAX)
        {
            fans_ = std::move(prev);
            return bad(where, "the fan list would be too long for the dashboard (" +
                                  std::to_string(proto::DASH_FAN_CONFIG_MAX) + " bytes as JSON) — "
                                  "shorten fan names, or remove a fan");
        }
        lastFrom_ = from;
        return true;
    }

    // apply a phone's edit (the shapes in the header comment). Validated as a
    // whole by applyList; `why` gets the phone's short reason on a refusal
    // (the journal gets the full line from bad()).
    bool applyJson(const json::Value& root, const std::string& where, std::string& why)
    {
        const json::Value* r = root.find("rev");
        const json::Value* fan = root.find("fan");
        const json::Value* edit = root.find("edit");
        const json::Value* add = root.find("add");
        const json::Value* del = root.find("del");

        for (auto& m : root.members)
            if (m.first != "rev" && m.first != "fan" && m.first != "edit" && m.first != "add" && m.first != "del")
            {
                why = "the edit wasn't understood";
                return bad(where + "." + m.first, "unknown key");
            }
        int ops = (fan || edit ? 1 : 0) + (add ? 1 : 0) + (del ? 1 : 0);
        if (ops != 1 || (fan && !edit) || (edit && !fan))
        {
            why = "the edit wasn't understood";
            return bad(where, "expected one of {fan, edit}, {add} or {del}");
        }
        if (!r || !r->isString() || r->text != rev())
        {
            why = "the fan list changed on the host since the phone read it — look again and retry";
            return bad(where, "stale edit (revision " + (r ? json::toString(*r) : std::string("none")) +
                                  ", the list is at " + rev() + ")");
        }

        json::Value arr;
        arr.type = json::Value::Type::Array;
        std::vector<int> from;
        for (size_t i = 0; i < fans_.size(); i++)
        {
            arr.items.push_back(configObject(fans_[i]));
            from.push_back((int)i);
        }

        auto index = [&](const json::Value* v, int& k) {
            if (!v->isNumber() || v->number != floor(v->number) || v->number < 0 ||
                v->number >= (double)fans_.size())
                return false;
            k = (int)v->number;
            return true;
        };

        int k = -1;
        if (fan)
        {
            if (!index(fan, k))
            {
                why = "no such fan";
                return bad(where + ".fan", "no such fan in the list");
            }
            std::vector<std::string> named;
            if (!fromShort(*edit, arr.items[k], named, where + ".edit"))
            {
                why = "the edit wasn't understood";
                return false;
            }
            fold(arr.items[k], named);
        }
        else if (add)
        {
            json::Value o;
            o.type = json::Value::Type::Object;
            std::vector<std::string> named;
            if (!fromShort(*add, o, named, where + ".add"))
            {
                why = "the new fan wasn't understood";
                return false;
            }
            arr.items.push_back(o);
            from.push_back(-1);
        }
        else
        {
            if (!index(del, k))
            {
                why = "no such fan";
                return bad(where + ".del", "no such fan in the list");
            }
            arr.items.erase(arr.items.begin() + k);
            from.erase(from.begin() + k);
        }

        if (!applyList(arr, from, where))
        {
            why = lastBad_;
            return false;
        }
        return true;
    }

    // a phone edit: parse, apply, write it into the config, and re-push the
    // standalone part if that moved. False (with a journal line, and the
    // reason for the phone in err_) changes nothing.
    bool applyEdit(const std::string& text, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        const std::string where = "fans (dashboard edit)";

        if (!writable() || !present_)
        {
            err_ = "the config file isn't writable";
            return bad(where, "refused — the config file is not writable, or has no fans block");
        }

        json::Value root;
        std::string perr;
        if (!json::parse(text, root, perr) || !root.isObject())
        {
            err_ = "the edit wasn't understood";
            return bad(where, "not a JSON object: " + perr);
        }

        std::string before = standaloneKey();
        std::string why;
        capture_ = true;
        lastBad_.clear();
        bool ok = applyJson(root, where, why);
        capture_ = false;
        if (!ok)
        {
            err_ = why.empty() ? "refused" : why;
            fprintf(stderr, "fans: dashboard edit refused — nothing changed\n");
            return false;
        }

        fprintf(stderr, "fans: live edit applied from the dashboard\n");
        if (standaloneKey() != before)
            pushStandalone(sinks);
        lastSent_ = -1e9; // send the new duties on the next tick, changed or not

        // the edit runs either way; a failed write is said by the writer and
        // turns the dashboard read-only
        if (!writeConfig())
            err_ = "applied, but the config file couldn't be written — it lasts until a restart";
        return true;
    }

    // the receiver's standalone settings as pushStandalone sends them: one
    // record per slot, an unused slot all 0xFF
    void standaloneBlob(uint8_t* p) const
    {
        for (int i = 0; i < CHANNELS; i++)
            fanwire::Header::unused().encode(p + i * proto::FAN_HEADER_LEN);
        for (auto& f : fans_)
            if (f.slot >= 0)
                f.wire().encode(p + f.slot * proto::FAN_HEADER_LEN);
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
    // wrote, in their order; a fan that is new since gets the canonical
    // order) and have the writer splice it over the block's bytes in the
    // config file; the rest of the file is untouched. The list is rebuilt from
    // the running fans, each continuing the file's object it came from (by
    // position: an edit keeps it, an add appends, a delete drops it — the
    // `from` map applyJson built). A failure is said and leaves the edit live
    // until the next restart (the writer turns read-only, so the phone stops
    // offering saves that can't stick).
    bool writeConfig()
    {
        json::Value arr;
        arr.type = json::Value::Type::Array;
        for (size_t i = 0; i < fans_.size(); i++)
        {
            const Fan& f = fans_[i];
            json::Value fv;
            fv.type = json::Value::Type::Object;
            int src = i < lastFrom_.size() ? lastFrom_[i] : -1;
            if (src >= 0 && (size_t)src < block_.items.size() && block_.items[src].isObject())
                fv = block_.items[src];

            cfgedit::member(fv, "name") = cfgedit::string(f.name);
            cfgedit::member(fv, "output") = cfgedit::string(f.output);
            cfgedit::member(fv, "input") = cfgedit::string(f.input);
            if (!f.hasCurve())
                cfgedit::erase(fv, "curve");
            else
            {
                // a curve where the file had none goes after input, where a
                // hand would write it, not at the end
                if (!fv.find("curve"))
                    for (size_t k = 0; k < fv.members.size(); k++)
                        if (fv.members[k].first == "input")
                        {
                            fv.members.insert(fv.members.begin() + k + 1,
                                              std::make_pair(std::string("curve"), json::Value()));
                            break;
                        }
                cfgedit::member(fv, "curve") = cfgedit::string(f.curveText);
            }
            cfgedit::member(fv, "fallback") = cfgedit::number(f.fallback);
            // no boost: null where the hand wrote the key, absent otherwise
            // (a host output's may not even be null-less — loadFan takes both)
            if (f.boost >= 0)
                cfgedit::member(fv, "boost") = cfgedit::number(f.boost);
            else if (fv.find("boost"))
                cfgedit::member(fv, "boost") = json::Value();
            // a tuning that stopped applying (the input moved) goes; one at
            // its default is written only where the hand already had it, so
            // a lean file stays lean
            for (auto& t : TUNINGS)
            {
                if (!t.applies(f))
                    cfgedit::erase(fv, t.key);
                else if (fv.find(t.key) || f.*t.field != t.dflt)
                    cfgedit::member(fv, t.key) = cfgedit::number(f.*t.field);
            }
            arr.items.push_back(fv);
        }

        block_ = arr;
        lastFrom_.clear();
        for (size_t i = 0; i < fans_.size(); i++)
            lastFrom_.push_back((int)i);
        return writer_->write({BLOCK}, block_, BLOCK);
    }

    std::vector<Fan> fans_;
    bool present_ = false;

    int stripPin_ = -1;      // the strip's data pin on the receiver (strip.pin)
    std::string sensors_;    // the top-level `sensors` spec (telemetry temp)
    json::Value block_;      // the fans block as written (writeConfig updates it)
    std::vector<int> lastFrom_; // fans_[i] continues block_.items[lastFrom_[i]]
    cfgedit::Writer* writer_ = nullptr; // the config file's editor; null = read-only
    pwmout::Claims* claims_ = nullptr;  // the host outputs' driver; null = never touch one
    std::string err_;        // why the last edit was refused, for the next push (once)
    std::string lastBad_;    // the last bad() while capture_ (the phone's short reason)
    bool capture_ = false;

    double watchUntil_ = 0;    // a phone watches until this time (0 = none)
    std::string tempPath_;
    float temp_ = 0;
    bool tempOk_ = false;
    std::string lastTelem_;
    double lastTelemSent_ = -1e9;
    bool warnedSize_ = false, warnedTelem_ = false;
    std::string lastSensors_;
    double lastSensorsSent_ = -1e9;
    bool warnedSensors_ = false;
    bool warnedOuts_ = false;        // host outputs left out of the catalogue (said once)
    bool warnedSensorsSize_ = false; // the catalogue over the ceiling even trimmed (said once)
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
