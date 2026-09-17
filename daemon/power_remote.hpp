#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <string>
#include <vector>

#include "config_edit.hpp"
#include "config_loader.hpp"
#include "protocol.hpp"
#include "sink.hpp"

// The power switch's half of the BLE dashboard (README "BLE remote", "Power
// switch"): the config's `power_switch` block as far as a phone may change
// it. The block is otherwise flash-time — pins and the enable go onto the
// receiver's `pwrcfg` partition with `make flash-pwr`, and stay there: a pin
// the machine's power hangs on is not re-pinned from a sofa. Its four
// TUNINGS, though, are values the switch's task reads on every poll, so they
// are pushed to the receiver at runtime (proto::CMD_PWR_TUNING) at startup,
// on a live reload and after an edit, the way the fans' standalone settings
// are — the receiver applies them at once and persists them, and this file
// stays the source of truth on a running machine. `short_press` is this
// daemon's own (the command a short press runs, daemon/output/serial_sink.hpp)
// and is applied to the link at once.
//
// Like the fans and the strip this is JSON text the receiver relays unread;
// the view goes out as CMD_PWR_CONFIG, an edit comes back as MSG_PWR_CONFIG,
// is validated here against the same ranges tools/pwrcfg.py enforces at
// flash time, written into the block through config_edit.hpp, pushed, and
// answered with the new view. The shape, all keys as the config spells them:
//
//     { "editable": true,                 // config writable and has the block
//       "hold_seconds": 2, "boot_timeout_seconds": 10,
//       "sense_low_mv": 800, "sense_high_mv": 2000,
//       "short_press": "systemctl poweroff" }   // or null
//
// An edit is a partial object of the same keys (not "editable"). A config
// with no power_switch block sends no view at all: the phone then talks to
// the receiver directly (its control op), and the daemon has no say.
namespace pwrcfg
{
class Remote
{
public:
    // the top-level config block this module owns; like the fans', nothing
    // in it feeds the strip (see fans::Controller::BLOCK)
    static constexpr const char* BLOCK = "power_switch";

    // read the block. cfg is held onto (its values are edited in place so the
    // running tree matches the file); writer is the config file's editor,
    // null = read-only.
    void load(Config& cfg, cfgedit::Writer* writer)
    {
        cfg_ = &cfg;
        writer_ = writer;
        const json::Value* block = cfg.root().find(BLOCK);
        hasBlock_ = block && block->isObject();
        tuningOk_ = false;
        enabled_ = false;
        shortPress_ = ""; // a block removed on reload takes its command with it
        if (!hasBlock_)
            return;

        if (const json::Value* en = block->find("enabled"))
            enabled_ = json::toBool(*en, false);
        auto num = [&](const char* key, double def) {
            const json::Value* v = block->find(key);
            return v && v->isNumber() ? v->number : def;
        };
        hold_ = num("hold_seconds", 2);
        boot_ = num("boot_timeout_seconds", 10);
        low_ = (int)num("sense_low_mv", 800);
        high_ = (int)num("sense_high_mv", 2000);
        if (const json::Value* sp = block->find("short_press"))
            if (sp->isString())
                shortPress_ = sp->text;

        // the four are the receiver's to run; a block pwrcfg.py refused at
        // flash time can still be hand-edited into nonsense afterwards, and
        // that must not be pushed (the receiver would refuse it anyway, but
        // silently, on a board with no console)
        std::string why;
        tuningOk_ = checkTuning(hold_, boot_, low_, high_, why);
        if (!tuningOk_)
            fprintf(stderr, "power_switch: %s — the receiver keeps its stored tunings, "
                            "and the dashboard's power settings are read-only\n", why.c_str());
    }

    bool writable() const { return hasBlock_ && tuningOk_ && writer_ && writer_->writable(); }
    const std::string& shortPress() const { return shortPress_; }

    // the tunings, for the receiver to run (see the header comment). Not
    // sent for a block with "enabled": false — the chip was flashed with the
    // switch off and would only log a refusal at every daemon start.
    void pushTuning(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (!hasBlock_ || !tuningOk_ || !enabled_)
            return;
        uint8_t p[proto::PWR_TUNING_LEN];
        const uint16_t v[4] = {(uint16_t)lround(hold_ * 1000), (uint16_t)lround(boot_ * 1000),
                               (uint16_t)low_, (uint16_t)high_};
        for (int i = 0; i < 4; i++)
        {
            p[2 * i] = (uint8_t)v[i];
            p[2 * i + 1] = (uint8_t)(v[i] >> 8);
        }
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_PWR_TUNING, p, sizeof p);
    }

    // the view, for the receiver to serve over GATT. Sent with no block too
    // ("present": false, which the page reads as no daemon view at all): the
    // receiver holds the last view it was given, and a daemon restarted after
    // the block was removed must not leave a stale, editable-looking one there.
    void pushConfig(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        std::string j = toJson();
        if (j.size() > WIRE_MAX)
        {
            // the receiver holds WIRE_MAX and drops a longer one unread; say so
            // rather than leave every phone save timing out for no visible reason
            if (!warnedSize_)
                fprintf(stderr, "power_switch: the dashboard view is %zu bytes as JSON, over the "
                                "receiver's %d — shorten short_press\n", j.size(), WIRE_MAX);
            warnedSize_ = true;
            return;
        }
        warnedSize_ = false;
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_PWR_CONFIG, (const uint8_t*)j.data(), (uint16_t)j.size());
    }

    // the short-press command onto the link(s) — at startup, after a reload,
    // after an edit; a no-op when unchanged
    void applyShortPress(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        for (auto& s : sinks)
            s->setShortPress(shortPress_);
    }

    // a msg frame from the receiver (protocol.hpp MSG_PWR_CONFIG): the
    // phone's edit. Main thread.
    void onMessage(uint8_t kind, const std::vector<uint8_t>& payload,
                   std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (kind != proto::MSG_PWR_CONFIG)
            return;
        if (applyEdit(std::string(payload.begin(), payload.end()), sinks))
            pushConfig(sinks); // the answer the phone waits for
    }

    std::string toJson() const
    {
        if (!hasBlock_)
            return "{\"present\":false,\"editable\":false}";
        char buf[200];
        snprintf(buf, sizeof buf,
                 "{\"editable\":%s,\"hold_seconds\":%g,\"boot_timeout_seconds\":%g,"
                 "\"sense_low_mv\":%d,\"sense_high_mv\":%d,\"short_press\":",
                 writable() ? "true" : "false", hold_, boot_, low_, high_);
        std::string j = buf;
        j += shortPress_.empty() ? "null" : "\"" + cfgedit::escape(shortPress_) + "\"";
        return j + "}";
    }

    // tools/pwrcfg.py's ranges, in one place on this side; public for the
    // config check (daemon/config_check.hpp), which asks the same question
    // of a file before the daemon runs on it
    static bool checkTuning(double hold, double boot, int low, int high, std::string& why)
    {
        char buf[160];
        if (!(hold >= 0.1 && hold <= 65.535))
            snprintf(buf, sizeof buf, "hold_seconds %g is not 0.1..65.535", hold);
        else if (!(boot >= 1 && boot <= 65.535))
            snprintf(buf, sizeof buf, "boot_timeout_seconds %g is not 1..65.535", boot);
        else if (low < 0 || low > 65535 || high < 0 || high > 65535)
            snprintf(buf, sizeof buf, "sense_low_mv/sense_high_mv must be 0..65535 mV");
        else if (low >= high)
            snprintf(buf, sizeof buf, "sense_low_mv %d must be below sense_high_mv %d: inverted "
                                      "hysteresis never settles, so the boot timeout would cut "
                                      "the PSU shortly after every power-on", low, high);
        else
            return true;
        why = buf;
        return false;
    }

private:
    static const int WIRE_MAX = 256; // the receiver's slot for CMD_PWR_CONFIG (protocol.hpp)

    bool bad(const std::string& where, const std::string& what)
    {
        fprintf(stderr, "%s: %s\n", where.c_str(), what.c_str());
        return false;
    }

    // parse and apply a phone's edit; false (with a journal line) changes
    // nothing. Validated as a whole — the merged tunings, not each key alone,
    // so a low raised past the old high with the high moving in the same edit
    // passes, and one raised past it alone does not. Once valid the edit is
    // pushed to the receiver and the link BEFORE the file write, so a write
    // that fails still leaves the running values and the pushed ones agreeing.
    bool applyEdit(const std::string& text, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        const std::string where = "power_switch (dashboard edit)";

        if (!writable())
            return bad(where, "refused — the config file is not writable, or its power_switch "
                              "block is missing or invalid");

        json::Value root;
        std::string err;
        if (!json::parse(text, root, err) || !root.isObject())
            return bad(where, "not a JSON object: " + err);

        double hold = hold_, boot = boot_;
        int low = low_, high = high_;
        std::string shortPress = shortPress_;
        bool tuningEdit = false, pressEdit = false;

        for (auto& m : root.members)
        {
            const std::string& k = m.first;
            const json::Value& v = m.second;
            if (k == "editable")
                continue; // echoed back by a lazy client; read-only
            if (k == "hold_seconds" || k == "boot_timeout_seconds")
            {
                if (!v.isNumber())
                    return bad(where + "." + k, "expected seconds");
                (k == "hold_seconds" ? hold : boot) = v.number;
                tuningEdit = true;
            }
            else if (k == "sense_low_mv" || k == "sense_high_mv")
            {
                if (!v.isNumber() || v.number != floor(v.number))
                    return bad(where + "." + k, "expected whole millivolts");
                (k == "sense_low_mv" ? low : high) = (int)v.number;
                tuningEdit = true;
            }
            else if (k == "short_press")
            {
                if (v.type == json::Value::Type::Null)
                    shortPress = "";
                else if (v.isString() && !v.text.empty())
                    shortPress = v.text;
                else
                    return bad(where + ".short_press", "expected a command string, or null to "
                                                       "ignore a short press");
                pressEdit = true;
            }
            else
                return bad(where + "." + k, "unknown key");
        }

        if (!tuningEdit && !pressEdit)
            return bad(where, "nothing to change");

        std::string why;
        if (tuningEdit && !checkTuning(hold, boot, low, high, why))
            return bad(where, why);

        // ---- everything validated: apply ----

        hold_ = hold;
        boot_ = boot;
        low_ = low;
        high_ = high;
        shortPress_ = shortPress;

        json::Value& block = cfgedit::member(cfg_->root(), BLOCK);
        cfgedit::member(block, "hold_seconds") = cfgedit::number(hold_);
        cfgedit::member(block, "boot_timeout_seconds") = cfgedit::number(boot_);
        cfgedit::member(block, "sense_low_mv") = cfgedit::number(low_);
        cfgedit::member(block, "sense_high_mv") = cfgedit::number(high_);
        cfgedit::member(block, "short_press") =
            shortPress_.empty() ? json::Value() : cfgedit::string(shortPress_);
        fprintf(stderr, "power_switch: live edit applied from the dashboard (hold %g s, boot "
                        "timeout %g s, sense %d..%d mV, short press %s%s%s)\n",
                hold_, boot_, low_, high_, shortPress_.empty() ? "ignored" : "\"",
                shortPress_.c_str(), shortPress_.empty() ? "" : "\"");

        pushTuning(sinks);
        applyShortPress(sinks);

        // a write that fails is said by the writer (and turns it read-only);
        // the edit stays live, but no answer goes back, so the phone's
        // timeout names the cause instead of a "saved" that won't survive
        return writer_->write({BLOCK}, block, BLOCK);
    }

    Config* cfg_ = nullptr;
    cfgedit::Writer* writer_ = nullptr;
    bool hasBlock_ = false;
    bool tuningOk_ = false;
    bool enabled_ = false;            // the block's "enabled": the chip was flashed with the switch on
    mutable bool warnedSize_ = false; // toJson is const; the warning is once per oversize

    double hold_ = 2, boot_ = 10;
    int low_ = 800, high_ = 2000;
    std::string shortPress_; // "" = null: a short press is ignored
};
} // namespace pwrcfg
