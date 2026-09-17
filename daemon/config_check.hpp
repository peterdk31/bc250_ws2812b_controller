#pragma once

// Would this config start the daemon — and is it the file its author meant to
// write? `led --check <config>` answers before `make install` restarts the
// service on it (Makefile `check`/`install`), so a key retired by a newer
// daemon, a typo'd block, or a color spelled "warm" costs an error line here
// instead of a crash loop in the journal, a silently ignored setting, or a
// white strip.
//
// Two kinds of finding. A PROBLEM is something wrong: what the daemon refuses
// to start on, and what it would run on but nobody could have meant (an
// unknown key inside a block with a fixed set, a value of the wrong type, a
// hex color that isn't one, a LED count the receiver can't hold). They fail
// the check. A NOTE is worth a look but may be intended (a top-level key the
// daemon doesn't read by name — effects see every top-level key as a shared
// default setting — or a rule no rule below can ever reach); notes don't fail
// it.
//
// The module composes the validation each block's owner already does
// (fans::Controller::load, loadRules, rec::record, pwrcfg::Remote::checkTuning,
// SerialSink::baudSupported) rather than re-deriving it, so those can't
// disagree with the daemon; the tables below only cover the blocks nothing
// else reads strictly. Nothing here opens a port, writes a file or talks to
// the receiver — the sensor lookups the loaders do (sysfs, i2c-dev probes)
// are reads and merely report "not found yet" where the box lacks them.
//
// The retired-keys table also lives here (it was main.cpp's) because startup,
// live reload and this check all want the same answer to "is this an old
// file?".

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include "config_loader.hpp"
#include "fans.hpp"
#include "power_remote.hpp"
#include "strip_remote.hpp"
#include "rules.hpp"
#include "serial_sink.hpp"
#include "recorder.hpp"

namespace cfgcheck
{

// The config's shape changed in Sep 2026 and there is no compatibility path
// (one file on one box): the "esp32" block dissolved into top-level keys and
// "sinks.serial" became "serial", with the power button moving into the
// "power_switch" block. Say exactly what to rename rather than running on a
// half-read file — a silently ignored "serial" block would mean a daemon that
// opens the wrong port with the wrong baud.
inline bool retiredKeys(const Config& cfg)
{
    const struct { const char* key; const char* now; } retired[] = {
        {"esp32", "its keys moved to the top level: \"target\", "
                  "\"host_timeout_ms\", \"power_on\", \"shutdown\""},
        {"sinks", "\"sinks\": { \"serial\": { ... } } is now just \"serial\": "
                  "{ ... }; power_button/power_button_command became "
                  "\"power_switch\": { \"short_press\": \"systemctl poweroff\" }"},
    };

    bool ok = true;
    for (auto& r : retired)
        if (cfg.root().find(r.key))
        {
            fprintf(stderr, "config: \"%s\" is no longer a key — %s\n", r.key, r.now);
            ok = false;
        }

    return ok;
}

// the receiver's frame buffer (firmware/main/render.hpp MAX_LEDS): a longer
// strip renders here but every recording upload is refused there
static const int RECEIVER_MAX_LEDS = 100;

// what a key's value must be. Int is a whole Number; Nullable variants take
// JSON null too (the config's "off" spelling for pins and commands).
enum class Kind { Str, Num, Int, Bool, Obj, Arr, StrOrNull, IntOrNull, StrOrNum, Any };

struct Field
{
    const char* name;
    Kind kind;
    const char* what; // one line, for the "expected ..." message
};

// the top-level keys the daemon reads by name (a module's BLOCK where one
// owns it). Any other top-level key is a note, not a problem: effects fall
// back to top-level keys for settings shared across rules (see EffectConfig).
static const Field TOP[] = {
    {"target", Kind::Str, "the chip to build and flash for, e.g. \"esp32c3\""},
    {"serial", Kind::Obj, "the link: { port, baud, debug_log }"},
    {"host_timeout_ms", Kind::Int, "milliseconds of silence before the receiver blanks (flash-time)"},
    {"power_on", Kind::Obj, "the boot recording: an effect or a sequence"},
    {"shutdown", Kind::Obj, "the shutdown recording: an effect or a sequence"},
    {pwrcfg::Remote::BLOCK, Kind::Obj, "the ATX power switch"},
    {"ble_remote", Kind::Obj, "the phone's power button and dashboard"},
    {fans::Controller::BLOCK, Kind::Obj, "the PWM fan headers"},
    {stripcfg::Remote::BLOCK, Kind::Obj, "the LED strip: { leds, pin, reverse, brightness, gamma, white_balance }"},
    {"crossfade_ms", Kind::Int, "the dissolve between effects, 0..65535 ms"},
    {"frame_ms", Kind::Int, "the default frame period every effect reads"},
    {"sensors", Kind::Any, "hwmon candidates: \"chip:label,...\" or an array of them"},
    {"rules", Kind::Arr, "the rules, first match wins"},
};

static const Field SERIAL[] = {
    {"port", Kind::Str, "a device path, or \"none\" to run headless"},
    {"baud", Kind::Int, "a supported baud rate (921600 by default)"},
    {"debug_log", Kind::Bool, "true to drain the receiver's log to the journal"},
};

static const Field STRIP[] = {
    {"leds", Kind::Int, "the LED count"},
    {"pin", Kind::Int, "the receiver GPIO driving the strip"},
    {"reverse", Kind::Bool, "true when LED 0 is at the far end"},
    {"brightness", Kind::Num, "0..1, linear"},
    {"gamma", Kind::StrOrNum, "one value (\"2.2\") or three (\"2.0 2.2 2.4\")"},
    {"white_balance", Kind::Str, "an RRGGBB neutral-white gain, ffffff = off"},
};

static const Field POWER_SWITCH[] = {
    {"enabled", Kind::Bool, "true when the switch is wired (flash-time)"},
    {"hold_seconds", Kind::Num, "seconds a press is held for a hard cut"},
    {"boot_timeout_seconds", Kind::Num, "seconds for the rail to come up"},
    {"sense_low_mv", Kind::Int, "the rail-off threshold in mV"},
    {"sense_high_mv", Kind::Int, "the rail-on threshold in mV"},
    {"pins", Kind::Obj, "{ ps_on, button, button_gnd, sense, led, wake }, each a GPIO or null (flash-time)"},
    {"short_press", Kind::StrOrNull, "the command a short press runs, or null to ignore it"},
};

static const Field BLE_REMOTE[] = {
    {"enabled", Kind::Bool, "true to advertise the phone remote (flash-time)"},
    {"name", Kind::Str, "the advertised device name"},
    {"token", Kind::Str, "the shared secret, 8-16 characters when enabled"},
};

static const Field RULE[] = {
    {"if", Kind::Str, "a condition, see README \"Conditions\""},
    {"effect", Kind::Str, "an effect name (led --list)"},
    {"hold", Kind::Num, "seconds since the last switch before this rule may take over"},
    {"settings", Kind::Obj, "the effect's settings"},
};

// one segment of a power_on/shutdown recording (or the whole block when it
// has no "sequence")
static const Field SEGMENT[] = {
    {"effect", Kind::Str, "a standalone effect name (led --list)"},
    {"settings", Kind::Obj, "the effect's settings"},
    {"record_seconds", Kind::Num, "how long an open-ended effect is recorded for"},
    {"loop", Kind::Bool, "true to loop the last segment instead of holding its last frame"},
};

class Checker
{
public:
    // the whole check; the process exit code (0 = no problems)
    int run(const char* path)
    {
        Config cfg;
        if (!cfg.load(path))
        {
            // parse and top-level errors print themselves; an unreadable
            // file doesn't
            FILE* f = fopen(path, "r");
            if (!f)
                fprintf(stderr, "%s: cannot read the file\n", path);
            else
                fclose(f);
            problems_++;
            return summary(path);
        }

        if (!retiredKeys(cfg))
        {
            problems_++;
            return summary(path); // the rest would only re-describe the old shape
        }

        const json::Value& root = cfg.root();

        checkKeys("config", root, TOP, sizeof TOP / sizeof *TOP, /*unknownIsProblem*/ false);
        checkScalars(cfg);
        checkBlock(root, "serial", SERIAL, sizeof SERIAL / sizeof *SERIAL);
        int beforeStrip = problems_;
        checkStrip(cfg);
        bool stripOk = problems_ == beforeStrip;
        checkPowerSwitch(cfg);
        checkBleRemote(cfg);
        checkSensors(cfg);

        // the owners' own validation — they print the problem themselves
        fans::Controller fans;
        if (!fans.load(cfg))
            problems_++;

        checkRules(cfg);

        // the recordings render through the strip's correction, so a broken
        // strip block (a white balance of 0) would only make them fail again
        if (stripOk)
        {
            Strip canvas = Strip::fromConfig(cfg);
            checkSlot(cfg, canvas, "power_on");
            checkSlot(cfg, canvas, "shutdown");
        }

        return summary(path);
    }

private:
    int problems_ = 0;
    int notes_ = 0;

    void problem(const std::string& where, const std::string& what)
    {
        fprintf(stderr, "%s: %s\n", where.c_str(), what.c_str());
        problems_++;
    }

    void note(const std::string& where, const std::string& what)
    {
        fprintf(stderr, "note: %s: %s\n", where.c_str(), what.c_str());
        notes_++;
    }

    int summary(const char* path)
    {
        std::string notes = notes_ ? std::to_string(notes_) + " note" + (notes_ > 1 ? "s" : "") : "";
        if (problems_ == 0)
            fprintf(stderr, "%s: OK%s\n", path, notes.empty() ? "" : (" (" + notes + ")").c_str());
        else
            fprintf(stderr, "%s: FAILED — %d problem%s%s\n", path, problems_, problems_ > 1 ? "s" : "",
                    notes.empty() ? "" : (", " + notes).c_str());
        return problems_ ? 1 : 0;
    }

    // ---- shape ----

    static bool isInt(const json::Value& v)
    {
        return v.isNumber() && v.number == floor(v.number);
    }

    static bool fits(const json::Value& v, Kind k)
    {
        using T = json::Value::Type;
        switch (k)
        {
            case Kind::Str:       return v.isString();
            case Kind::Num:       return v.isNumber();
            case Kind::Int:       return isInt(v);
            case Kind::Bool:      return v.type == T::Bool;
            case Kind::Obj:       return v.isObject();
            case Kind::Arr:       return v.isArray();
            case Kind::StrOrNull: return v.isString() || v.type == T::Null;
            case Kind::IntOrNull: return isInt(v) || v.type == T::Null;
            case Kind::StrOrNum:  return v.isString() || v.isNumber();
            case Kind::Any:       return true;
        }
        return false;
    }

    static const char* kindName(Kind k)
    {
        switch (k)
        {
            case Kind::Str:       return "a string";
            case Kind::Num:       return "a number";
            case Kind::Int:       return "a whole number";
            case Kind::Bool:      return "true or false";
            case Kind::Obj:       return "an object { ... }";
            case Kind::Arr:       return "an array [ ... ]";
            case Kind::StrOrNull: return "a string or null";
            case Kind::IntOrNull: return "a whole number or null";
            case Kind::StrOrNum:  return "a string or a number";
            case Kind::Any:       return "a value";
        }
        return "";
    }

    // edit distance, for "did you mean" on a typo'd key
    static size_t distance(const std::string& a, const std::string& b)
    {
        std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++)
            prev[j] = j;
        for (size_t i = 1; i <= a.size(); i++)
        {
            cur[0] = i;
            for (size_t j = 1; j <= b.size(); j++)
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                                   prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            std::swap(prev, cur);
        }
        return prev[b.size()];
    }

    static std::string didYouMean(const std::string& key, const Field* fields, size_t n)
    {
        const Field* best = nullptr;
        size_t bestD = 3; // 2 edits at most
        for (size_t i = 0; i < n; i++)
        {
            size_t d = distance(key, fields[i].name);
            if (d < bestD)
            {
                bestD = d;
                best = &fields[i];
            }
        }
        return best ? std::string(" — did you mean \"") + best->name + "\"?" : "";
    }

    // every member of `obj` against the table: a wrong type is a problem; a
    // key not in the table is a problem for a block with a fixed set, a note
    // for the top level
    void checkKeys(const std::string& where, const json::Value& obj, const Field* fields, size_t n,
                   bool unknownIsProblem = true)
    {
        for (auto& m : obj.members)
        {
            const Field* f = nullptr;
            for (size_t i = 0; i < n; i++)
                if (m.first == fields[i].name)
                    f = &fields[i];

            std::string at = where + "." + m.first;
            if (!f)
            {
                std::string hint = didYouMean(m.first, fields, n);
                if (unknownIsProblem)
                    problem(at, "unknown key" + hint);
                else
                    note(at, "not a key the daemon reads by name (effects see it as a "
                             "shared default setting)" + hint);
            }
            else if (!fits(m.second, f->kind))
                problem(at, std::string("expected ") + kindName(f->kind) + ": " + f->what);
        }
    }

    // a top-level block with a fixed key set; absent is fine
    const json::Value* checkBlock(const json::Value& root, const char* name, const Field* fields, size_t n)
    {
        const json::Value* b = root.find(name);
        if (!b || !b->isObject())
            return nullptr; // its type was checked against TOP
        checkKeys(name, *b, fields, n);
        return b;
    }

    // ---- values ----

    // "RRGGBB" / "#RRGGBB"; the strict form of what parseHexColor and
    // color::Gradient accept, since both silently make something of anything
    static bool hex6(std::string t)
    {
        if (!t.empty() && t[0] == '#')
            t.erase(0, 1);
        return t.size() == 6 && t.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    }

    // a palette: hex colors separated by commas and/or spaces, at least one
    static bool hexList(const std::string& text)
    {
        size_t n = 0, start = 0;
        while (start < text.size())
        {
            size_t end = text.find_first_of(", ", start);
            if (end == std::string::npos)
                end = text.size();
            if (end > start)
            {
                if (!hex6(text.substr(start, end - start)))
                    return false;
                n++;
            }
            start = end + 1;
        }
        return n > 0;
    }

    static bool endsWith(const std::string& s, const char* suffix)
    {
        size_t n = strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    void checkScalars(const Config& cfg)
    {
        const json::Value* v;
        if ((v = cfg.find("host_timeout_ms")) && isInt(*v) && v->number < 0)
            problem("config.host_timeout_ms", "expected milliseconds, 0 or more");
        if ((v = cfg.find("crossfade_ms")) && isInt(*v) && (v->number < 0 || v->number > 65535))
            problem("config.crossfade_ms", "expected 0..65535 ms (the wire carries 16 bits)");
        if ((v = cfg.find("frame_ms")) && isInt(*v) && v->number < 1)
            problem("config.frame_ms", "expected a frame period of 1 ms or more");
        if ((v = cfg.find("serial.baud")) && isInt(*v) && !SerialSink::baudSupported((int)v->number))
            problem("serial.baud", "not a baud rate the link can open (9600 .. 2000000, "
                                   "the standard steps; 921600 is the default)");
    }

    void checkStrip(const Config& cfg)
    {
        const char* block = stripcfg::Remote::BLOCK;
        const json::Value* b = checkBlock(cfg.root(), block, STRIP, sizeof STRIP / sizeof *STRIP);
        if (!b)
            return;
        std::string where = block;

        const json::Value* v;
        if ((v = b->find("leds")) && isInt(*v))
        {
            if (v->number < 1)
                problem(where + ".leds", "expected 1 or more");
            else if (v->number > RECEIVER_MAX_LEDS)
                problem(where + ".leds", "over the receiver's " + std::to_string(RECEIVER_MAX_LEDS) +
                                             " (firmware MAX_LEDS) — it would refuse every "
                                             "boot/shutdown recording");
        }
        if ((v = b->find("pin")) && isInt(*v) && v->number < 0)
            problem(where + ".pin", "expected a GPIO number");
        if ((v = b->find("brightness")) && v->isNumber() && (v->number < 0 || v->number > 1))
            problem(where + ".brightness", "expected 0..1 (linear)");
        if ((v = b->find("gamma")) && (v->isString() || v->isNumber()))
        {
            float g[3];
            int n = sscanf(json::toString(*v).c_str(), "%f %f %f", &g[0], &g[1], &g[2]);
            bool ok = (n == 1 || n == 3);
            for (int i = 0; ok && i < n; i++)
                ok = g[i] > 0;
            if (!ok)
                problem(where + ".gamma", "expected one positive value (\"2.2\") or three "
                                          "(\"2.0 2.2 2.4\")");
        }
        if ((v = b->find("white_balance")) && v->isString() && !hex6(v->text))
            problem(where + ".white_balance", "expected an RRGGBB color");
    }

    void checkPowerSwitch(const Config& cfg)
    {
        const char* block = pwrcfg::Remote::BLOCK;
        const json::Value* b = checkBlock(cfg.root(), block, POWER_SWITCH,
                                          sizeof POWER_SWITCH / sizeof *POWER_SWITCH);
        if (!b)
            return;
        std::string where = block;

        // the pin names are the flasher's (tools/pwrcfg.py); each value is a
        // GPIO or null
        if (const json::Value* pins = b->find("pins"))
            if (pins->isObject())
                for (auto& m : pins->members)
                    if (!fits(m.second, Kind::IntOrNull))
                        problem(where + ".pins." + m.first, "expected a GPIO number, or null for not wired");

        // the four tunings as pwrcfg::Remote reads them (its defaults for the
        // absent ones); the daemon runs on a bad set but never pushes it
        auto num = [&](const char* key, double def) {
            const json::Value* v = b->find(key);
            return v && v->isNumber() ? v->number : def;
        };
        std::string why;
        if (!pwrcfg::Remote::checkTuning(num("hold_seconds", 2), num("boot_timeout_seconds", 10),
                                         (int)num("sense_low_mv", 800), (int)num("sense_high_mv", 2000), why))
            problem(where, why);

        if (const json::Value* sp = b->find("short_press"))
            if (sp->isString() && sp->text.empty())
                problem(where + ".short_press", "an empty command — write null to ignore a short press");
    }

    void checkBleRemote(const Config& cfg)
    {
        const json::Value* b = checkBlock(cfg.root(), "ble_remote", BLE_REMOTE,
                                          sizeof BLE_REMOTE / sizeof *BLE_REMOTE);
        if (!b)
            return;

        bool enabled = cfg.getBool("ble_remote.enabled", false);
        const json::Value* token = b->find("token");
        const json::Value* name = b->find("name");
        if (enabled)
        {
            size_t n = token && token->isString() ? token->text.size() : 0;
            if (n < 8 || n > 16)
                problem("ble_remote.token", "8-16 characters are required while enabled (openssl "
                                            "rand -hex 6 makes one) — tools/blecfg.py refuses "
                                            "to flash without");
            if (name && name->isString() && name->text.empty())
                problem("ble_remote.name", "the advertised name can't be empty");
        }
    }

    void checkSensors(const Config& cfg)
    {
        const json::Value* s = cfg.root().find("sensors");
        if (!s)
            return;
        if (s->isString())
            return;
        if (!s->isArray())
        {
            problem("config.sensors", "expected \"chip:label,...\" or an array of such strings");
            return;
        }
        for (size_t i = 0; i < s->items.size(); i++)
            if (!s->items[i].isString())
                problem("config.sensors[" + std::to_string(i) + "]", "expected a \"chip:label\" string");
    }

    // every color-ish setting under `v`, recursively (cycle nests whole
    // effect blocks): "color"/"*_color" take one RRGGBB, "palette"/"*_palette"
    // a list. The effects would quietly render white, or the first hex-looking
    // fragment, for anything else.
    void checkColors(const std::string& where, const json::Value& v)
    {
        if (v.isArray())
        {
            for (size_t i = 0; i < v.items.size(); i++)
                checkColors(where + "[" + std::to_string(i) + "]", v.items[i]);
            return;
        }
        if (!v.isObject())
            return;
        for (auto& m : v.members)
        {
            const std::string& k = m.first;
            bool single = k == "color" || endsWith(k, "_color");
            bool list = k == "palette" || endsWith(k, "_palette");
            std::string at = where + "." + k;
            if (single || list)
            {
                if (!m.second.isString())
                    problem(at, "write colors as a quoted \"RRGGBB\" string");
                else if (single && !hex6(m.second.text))
                    problem(at, "expected one RRGGBB color");
                else if (list && !hexList(m.second.text))
                    problem(at, "expected RRGGBB colors separated by commas, e.g. \"3a0a00,ff5a0a\"");
            }
            else
                checkColors(at, m.second);
        }
    }

    void checkRules(const Config& cfg)
    {
        const json::Value* list = cfg.root().find("rules");
        if (list && list->isArray())
        {
            int catchAll = -1;
            size_t n = list->items.size();
            for (size_t i = 0; i < n; i++)
            {
                const json::Value& r = list->items[i];
                std::string where = "rules[" + std::to_string(i) + "]";
                if (!r.isObject())
                {
                    problem(where, "expected an object { \"if\", \"effect\", \"settings\" }");
                    continue;
                }
                checkKeys(where, r, RULE, sizeof RULE / sizeof *RULE);

                const json::Value* hold = r.find("hold");
                if (hold && hold->isNumber() && hold->number < 0)
                    problem(where + ".hold", "expected seconds, 0 or more");

                if (const json::Value* s = r.find("settings"))
                    checkColors(where + ".settings", *s);

                // a rule nothing below can beat: a catch-all is fine as the
                // last rule and dead weight anywhere else
                const json::Value* cond = r.find("if");
                bool always = !cond || (cond->isString() && cond->text == "always");
                if (catchAll >= 0 && (int)i == catchAll + 1)
                    note(i + 1 < n ? where + "..rules[" + std::to_string(n - 1) + "]" : where,
                         "never reached — rules[" + std::to_string(catchAll) + "] matches everything first");
                else if (catchAll < 0 && always)
                    catchAll = (int)i;
            }
        }

        // the rules' own loader: conditions, effect names, shape
        std::vector<Rule> rules;
        if (!loadRules(cfg, rules))
            problems_++;
    }

    // does this segment's effect light a single pixel while it records? The
    // receiver renders nothing itself, so cycle (fed by the rules' JSON) and
    // the effects with no host data to show are dark in a slot — a heuristic:
    // an effect with a baseline glow (load) passes. Rendered as rec::record
    // does — a simulated clock, the effect's own frame rate — up to
    // record_seconds (the recorder's default of 6) or the effect finishing.
    static bool segmentLights(const Config& cfg, Strip canvas, const json::Value& seg)
    {
        std::unique_ptr<Effect> effect = createEffect(json::toString(*seg.find("effect")));
        if (!effect)
            return true; // the recorder reports the unknown name itself
        effect->init(EffectConfig(cfg, seg.find("settings")), canvas.size());
        const json::Value* rs = seg.find("record_seconds");
        float cap = rs ? json::toFloat(*rs, 6.0f) : 6.0f;
        float dt = effect->frameDelayMs() / 1000.0f;
        float t = 0;
        do // the recorder always takes the first frame, whatever the cap
        {
            canvas.beginFrame();
            effect->render(canvas, t);
            const std::vector<uint8_t>& wire = canvas.endFrame();
            if (std::any_of(wire.begin() + proto::PIX_HEADER, wire.end() - 1,
                            [](uint8_t b) { return b != 0; }))
                return true;
            t += dt;
        } while (!effect->finished() && t < cap);
        return false;
    }

    // a power_on/shutdown block: its segments' shape, then the recording
    // itself, rendered as the daemon would at startup (deterministic and
    // fast — the effects run against a simulated clock)
    void checkSlot(const Config& cfg, const Strip& canvas, const char* slot)
    {
        const json::Value* b = cfg.root().find(slot);
        if (!b || !b->isObject())
            return;
        std::string where = slot;
        int before = problems_;

        std::vector<std::pair<std::string, const json::Value*>> segs;
        const json::Value* seq = b->find("sequence");
        if (seq)
        {
            if (!seq->isArray())
            {
                problem(where + ".sequence", "expected an array of segments");
                return;
            }
            for (size_t i = 0; i < seq->items.size(); i++)
                segs.push_back({where + ".sequence[" + std::to_string(i) + "]", &seq->items[i]});
            for (auto& m : b->members)
                if (m.first != "sequence")
                    problem(where + "." + m.first, "a block with a \"sequence\" holds nothing else — "
                                                   "move this into a segment");
            if (segs.empty())
                problem(where + ".sequence", "no segments — nothing to record");
        }
        else
            segs.push_back({where, b});

        for (size_t i = 0; i < segs.size(); i++)
        {
            const std::string& at = segs[i].first;
            const json::Value& seg = *segs[i].second;
            if (!seg.isObject())
            {
                problem(at, "expected a segment { \"effect\", \"record_seconds\", \"settings\" }");
                continue;
            }
            checkKeys(at, seg, SEGMENT, sizeof SEGMENT / sizeof *SEGMENT);
            if (!seg.find("effect"))
                problem(at, "missing \"effect\" — the recorder skips a segment without one");
            const json::Value* rs = seg.find("record_seconds");
            if (rs && rs->isNumber() && rs->number <= 0)
                problem(at + ".record_seconds", "expected seconds above 0");
            if (seg.find("loop") && i + 1 < segs.size())
                note(at + ".loop", "only the last segment's loop counts; earlier ones always play once");
            if (const json::Value* s = seg.find("settings"))
                checkColors(at + ".settings", *s);
            const json::Value* eff = seg.find("effect");
            bool secondsOk = !rs || !rs->isNumber() || rs->number > 0;
            if (secondsOk && eff && eff->isString() && createEffect(eff->text) &&
                !segmentLights(cfg, canvas, seg))
                problem(at + ".effect", "\"" + eff->text + "\" records dark — a slot needs a "
                                        "standalone animation (cycle and the effects fed by host "
                                        "data have nothing to show here)");
        }

        if (problems_ > before)
            return; // a broken segment would just fail again below, less clearly

        rec::Recording r;
        if (!rec::record(cfg, canvas, slot, r) || !r.valid())
        {
            problem(where, "could not be recorded (see above)");
            return;
        }
        fprintf(stderr, "%s: records %u frames at %u ms (%.1f s, %zu KB on the receiver)\n", slot,
                r.frameCount, r.frameMs, r.frameCount * r.frameMs / 1000.0,
                ((size_t)r.frameCount * r.count * 6 + 1023) / 1024);
    }
};

// `led --check <config>`: the exit code
inline int run(const char* path)
{
    Checker c;
    return c.run(path);
}

} // namespace cfgcheck
