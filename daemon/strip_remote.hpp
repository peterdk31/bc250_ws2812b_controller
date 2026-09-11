#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>

#include "condition.hpp"
#include "config_edit.hpp"
#include "config_loader.hpp"
#include "protocol.hpp"
#include "sink.hpp"
#include "strip.hpp"

// The strip's half of the BLE dashboard (README "BLE remote"): what the phone
// sees and edits of the config's `strip` block, and the "scenes" — the rules
// whose condition is a bare `file:` path, the config's own external control
// surface (`touch /tmp/led-static-color`), which the phone can flip without a shell.
// Its reason to exist is tuning the colors from the couch: switch the "tune"
// scene on (a `solid` rule), move white balance and gamma while looking at
// the strip, switch it off — README "Tuning the colors".
//
// Like the fan controller (fans.hpp) this is JSON text the receiver relays
// unread. The view goes out as CMD_STRIP_CONFIG (toJson) at startup, when a
// scene's file appears or disappears, and after every applied edit — which
// is also how a MSG_STRIP_CONFIG edit gets its answer. An edit is validated
// here, applied to the running strip at once (main.cpp calls applyTo on the
// live canvas), and written into the config through config_edit.hpp — into
// the `strip` block, or into the scene's rule for a color. Toggling a scene
// writes nothing: it creates or removes the rule's file, and the rule engine
// notices on its next tick like it would a shell's touch.
//
// The JSON shape the wire and the phone's edits use:
//
//     { "editable": true,               // config writable and has a strip block
//       "leds": 33, "pin": 4,           // read-only: wiring facts
//       "reverse": false, "brightness": 1, "gamma": "2.2", "white_balance": "ffb0f0",
//       "scenes": [
//         { "p": "/tmp/led-static-color", "e": "solid", "on": false,
//           "color": "ffffff", "l": 1 },  // color/l only when the rule's
//         { "p": "/tmp/led-night", "e": "drift", "on": true } ] }   // settings have them
//
// An edit is a partial object: any of reverse / brightness / gamma /
// white_balance, and/or "scenes": [{ "p": path, "on": bool }] or
// [{ "p": path, "color": "rrggbb", "l": 0.25 }] (either or both). `gamma`
// is the config's own notation — one value, or three space-separated.
namespace stripcfg
{
class Remote
{
public:
    // read the strip block and collect the scenes from the rules. cfg is
    // held onto (its values are edited in place so the running tree matches
    // the file); writer is the config file's editor, null = read-only.
    void load(Config& cfg, cfgedit::Writer* writer)
    {
        cfg_ = &cfg;
        writer_ = writer;
        scenes_.clear();

        const json::Value* block = cfg.root().find("strip");
        hasBlock_ = block && block->isObject();

        leds_ = cfg.getInt("strip.leds", 10);
        pin_ = cfg.getInt("strip.pin", 13);
        reverse_ = cfg.getBool("strip.reverse", false);
        brightness_ = cfg.getFloat("strip.brightness", 0.1f);
        if (const json::Value* b = cfg.find("strip.brightness"))
            if (b->isNumber())
                brightness_ = b->number; // the exact double, see Scene::level
        gammaText_ = cfg.get("strip.gamma", "2.2");
        Strip::parseGamma(gammaText_, gr_, gg_, gb_);
        gammaText_ = gammaToText(gr_, gg_, gb_);
        wb_ = Strip::parseColor(cfg.get("strip.white_balance", "ffffff")) & 0xFFFFFF;

        const json::Value* rules = cfg.root().find("rules");
        if (rules && rules->isArray())
            for (size_t i = 0; i < rules->items.size(); i++)
            {
                const json::Value& r = rules->items[i];
                const json::Value* cond = r.find("if");
                const json::Value* eff = r.find("effect");
                if (!cond || !cond->isString() || !eff || !eff->isString())
                    continue;
                Scene s;
                if (!fileSwitchPath(cond->text, s.path))
                    continue; // a compound condition isn't a switch
                s.rule = i;
                s.effect = eff->text;
                if (const json::Value* st = r.find("settings"))
                    if (st->isObject())
                    {
                        const json::Value* col = st->find("color");
                        if (col && col->isString())
                        {
                            s.hasColor = true;
                            s.color = Strip::parseColor(col->text) & 0xFFFFFF;
                        }
                        const json::Value* lv = st->find("level");
                        if (lv && lv->isNumber())
                        {
                            s.hasLevel = true;
                            s.level = lv->number;
                        }
                    }
                s.on = exists(s.path);
                scenes_.push_back(s);
            }
    }

    bool writable() const { return hasBlock_ && writer_ && writer_->writable(); }

    // put the running values onto the live canvas — at startup they are the
    // same values Strip::fromConfig read; after an edit they are the new ones
    void applyTo(Strip& strip) const
    {
        strip.setGamma(gr_, gg_, gb_);
        strip.setWhiteBalance(wb_);
        strip.setBrightness((float)brightness_);
        strip.setReversed(reverse_);
    }

    // the view, for the receiver to serve over GATT
    void pushConfig(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        std::string j = toJson();
        for (auto& s : sinks)
            s->sendCommand(proto::CMD_STRIP_CONFIG, (const uint8_t*)j.data(), (uint16_t)j.size());
    }

    // the scenes' files, re-checked on the rules' tick: a shell's touch (or
    // the rule engine noticing ours) changes the view the phone shows
    void tick(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (scenes_.empty() || now - lastScan_ < 0.5)
            return;
        lastScan_ = now;
        if (rescan())
            pushConfig(sinks);
    }

    // a msg frame from the receiver (protocol.hpp MSG_STRIP_CONFIG): the
    // phone's edit. Main thread.
    void onMessage(uint8_t kind, const std::vector<uint8_t>& payload,
                   std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (kind != proto::MSG_STRIP_CONFIG)
            return;
        if (applyEdit(std::string(payload.begin(), payload.end())))
        {
            rescan();
            pushConfig(sinks); // the answer the phone waits for
        }
    }

    // did an edit change brightness/gamma/white balance/reverse since the
    // last call? The main loop then applies it to the canvas (applyTo) and
    // re-records the boot/shutdown animations, which bake the correction in.
    bool takeStripChanged()
    {
        bool c = stripChanged_;
        stripChanged_ = false;
        return c;
    }

    // which rules had their settings edited (a scene's color or level) since
    // the last call — indices into the config's rules. The effect built from
    // one of them has to restart to show it; the caller knows which is running.
    std::vector<size_t> takeRulesChanged()
    {
        std::vector<size_t> out = std::move(changedRules_);
        changedRules_.clear();
        return out;
    }

    std::string toJson() const
    {
        char buf[160];
        std::string j = writable() ? "{\"editable\":true" : "{\"editable\":false";
        snprintf(buf, sizeof buf,
                 ",\"leds\":%d,\"pin\":%d,\"reverse\":%s,\"brightness\":%g,\"gamma\":\"%s\","
                 "\"white_balance\":\"%06x\",\"scenes\":[",
                 leds_, pin_, reverse_ ? "true" : "false", brightness_,
                 gammaText_.c_str(), wb_);
        j += buf;

        bool first = true;
        for (auto& s : scenes_)
        {
            std::string e = (first ? "{\"p\":\"" : ",{\"p\":\"") + cfgedit::escape(s.path) +
                            "\",\"e\":\"" + cfgedit::escape(s.effect) + "\",\"on\":" +
                            (s.on ? "true" : "false");
            if (s.hasColor)
                snprintf(buf, sizeof buf, ",\"color\":\"%06x\"", s.color), e += buf;
            if (s.hasLevel)
                snprintf(buf, sizeof buf, ",\"l\":%g", s.level), e += buf;
            e += "}";
            if (j.size() + e.size() + 2 > WIRE_MAX)
            {
                // a GATT attribute's ceiling: the phone sees the first scenes
                if (!warnedSize_)
                    fprintf(stderr, "strip: too many scene rules for the dashboard's %d bytes "
                                    "— showing the first %zu\n", WIRE_MAX,
                            (size_t)(&s - &scenes_[0]));
                warnedSize_ = true;
                break;
            }
            j += e;
            first = false;
        }
        return j + "]}";
    }

private:
    static const int WIRE_MAX = 512; // a GATT attribute's ceiling = the receiver's buffer

    struct Scene
    {
        size_t rule = 0;    // index into the config's rules
        std::string path;
        std::string effect;
        bool hasColor = false;
        uint32_t color = 0xFFFFFF;
        bool hasLevel = false;
        double level = 1;   // as the config holds it (a double, so the tree
                            // and the file re-parse to the same value)
        bool on = false;    // the file exists
    };

    static bool exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

    // re-stat every scene; true when one changed
    bool rescan()
    {
        bool changed = false;
        for (auto& s : scenes_)
        {
            bool on = exists(s.path);
            changed |= on != s.on;
            s.on = on;
        }
        return changed;
    }

    static std::string gammaToText(float r, float g, float b)
    {
        char buf[64];
        if (r == g && g == b)
            snprintf(buf, sizeof buf, "%g", (double)r);
        else
            snprintf(buf, sizeof buf, "%g %g %g", (double)r, (double)g, (double)b);
        return buf;
    }

    static bool hex6(const std::string& t, uint32_t& out)
    {
        std::string v = t;
        if (!v.empty() && v[0] == '#')
            v.erase(0, 1);
        if (v.size() != 6 || v.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return false;
        out = (uint32_t)strtoul(v.c_str(), nullptr, 16);
        return true;
    }

    bool bad(const std::string& where, const std::string& what)
    {
        fprintf(stderr, "%s: %s\n", where.c_str(), what.c_str());
        return false;
    }

    // parse and apply a phone's edit; false (with a journal line) changes
    // nothing. Validated as a whole before anything is applied.
    bool applyEdit(const std::string& text)
    {
        const std::string where = "strip (dashboard edit)";

        if (!writable())
            return bad(where, "refused — the config file is not writable, or has no strip block");

        json::Value root;
        std::string err;
        if (!json::parse(text, root, err) || !root.isObject())
            return bad(where, "not a JSON object: " + err);

        // the strip values, starting from what runs
        bool reverse = reverse_;
        double brightness = brightness_;
        float gr = gr_, gg = gg_, gb = gb_;
        uint32_t wb = wb_;
        bool stripEdit = false;

        struct SceneEdit
        {
            std::string path;
            int on = -1;        // -1 = leave
            bool setColor = false;
            uint32_t color = 0;
            bool setLevel = false;
            double level = 0;
        };
        std::vector<SceneEdit> sceneEdits;

        for (auto& m : root.members)
        {
            const std::string& k = m.first;
            const json::Value& v = m.second;
            if (k == "editable" || k == "leds" || k == "pin")
                continue; // echoed back by a lazy client; read-only
            if (k == "reverse")
            {
                if (v.type != json::Value::Type::Bool)
                    return bad(where + ".reverse", "expected true or false");
                reverse = v.boolean;
                stripEdit = true;
            }
            else if (k == "brightness")
            {
                if (!v.isNumber() || v.number < 0 || v.number > 1)
                    return bad(where + ".brightness", "expected 0..1");
                brightness = v.number;
                stripEdit = true;
            }
            else if (k == "gamma")
            {
                if (!v.isString() && !v.isNumber())
                    return bad(where + ".gamma", "expected \"2.2\" or \"r g b\"");
                float r, g, b;
                std::string t = json::toString(v);
                float probe[3];
                int n = sscanf(t.c_str(), "%f %f %f", &probe[0], &probe[1], &probe[2]);
                if (n != 1 && n != 3)
                    return bad(where + ".gamma", "expected one value or three");
                Strip::parseGamma(t, r, g, b);
                for (float x : {r, g, b})
                    if (!(x >= 0.5f && x <= 5.0f))
                        return bad(where + ".gamma", "each value 0.5..5");
                gr = r; gg = g; gb = b;
                stripEdit = true;
            }
            else if (k == "white_balance")
            {
                if (!v.isString() || !hex6(v.text, wb))
                    return bad(where + ".white_balance", "expected six hex digits, rrggbb");
                stripEdit = true;
            }
            else if (k == "scenes")
            {
                if (!v.isArray())
                    return bad(where + ".scenes", "expected an array");
                for (auto& item : v.items)
                {
                    if (!item.isObject())
                        return bad(where + ".scenes", "expected objects");
                    SceneEdit e;
                    const json::Value* p = item.find("p");
                    if (!p || !p->isString())
                        return bad(where + ".scenes", "each needs its path, \"p\"");
                    e.path = p->text;
                    bool known = false, hasColor = false, hasLevel = false;
                    for (auto& s : scenes_)
                        if (s.path == e.path)
                        {
                            known = true;
                            hasColor |= s.hasColor;
                            hasLevel |= s.hasLevel;
                        }
                    if (!known)
                        return bad(where + ".scenes", "no file: rule for " + e.path);
                    for (auto& f : item.members)
                    {
                        const std::string& fk = f.first;
                        const json::Value& fv = f.second;
                        if (fk == "p" || fk == "e")
                            continue;
                        if (fk == "on" && fv.type == json::Value::Type::Bool)
                            e.on = fv.boolean ? 1 : 0;
                        else if (fk == "color" && fv.isString() && hasColor)
                        {
                            if (!hex6(fv.text, e.color))
                                return bad(where + ".scenes.color", "expected six hex digits, rrggbb");
                            e.setColor = true;
                        }
                        else if (fk == "l" && fv.isNumber() && hasLevel)
                        {
                            if (fv.number < 0 || fv.number > 1)
                                return bad(where + ".scenes.l", "expected 0..1");
                            e.setLevel = true;
                            e.level = fv.number;
                        }
                        else
                            return bad(where + ".scenes." + fk, "not understood for " + e.path);
                    }
                    sceneEdits.push_back(e);
                }
            }
            else
                return bad(where + "." + k, "unknown key");
        }

        if (!stripEdit && sceneEdits.empty())
            return bad(where, "nothing to change");

        // ---- everything validated: apply ----

        // a write that fails is said by the writer (and turns it read-only);
        // the edit stays live, but no answer goes back, so the phone's
        // timeout names the cause instead of a "saved" that won't survive
        bool written = true;

        if (stripEdit)
        {
            reverse_ = reverse;
            brightness_ = brightness;
            gr_ = gr; gg_ = gg; gb_ = gb;
            gammaText_ = gammaToText(gr, gg, gb);
            wb_ = wb;
            stripChanged_ = true;

            json::Value& block = cfgedit::member(cfg_->root(), "strip");
            cfgedit::member(block, "reverse") = cfgedit::boolean(reverse_);
            cfgedit::member(block, "brightness") = cfgedit::number(brightness_);
            cfgedit::member(block, "gamma") = cfgedit::string(gammaText_);
            char hex[8];
            snprintf(hex, sizeof hex, "%06x", wb_);
            cfgedit::member(block, "white_balance") = cfgedit::string(hex);
            fprintf(stderr, "strip: live edit applied from the dashboard (brightness %g, gamma %s, "
                            "white balance %s%s)\n",
                    brightness_, gammaText_.c_str(), hex, reverse_ ? ", reversed" : "");
            written &= writer_->write({"strip"}, block, "strip");
        }

        for (auto& e : sceneEdits)
        {
            for (auto& s : scenes_)
            {
                if (s.path != e.path)
                    continue;
                if ((e.setColor && s.hasColor) || (e.setLevel && s.hasLevel))
                {
                    // the rule exists with these settings: the scene came from it
                    json::Value& rule = cfgedit::member(cfg_->root(), "rules").items[s.rule];
                    json::Value& st = cfgedit::member(rule, "settings");
                    std::string idx = std::to_string(s.rule);
                    if (e.setColor && s.hasColor)
                    {
                        s.color = e.color;
                        char hex[8];
                        snprintf(hex, sizeof hex, "%06x", s.color);
                        cfgedit::member(st, "color") = cfgedit::string(hex);
                        fprintf(stderr, "strip: scene %s color -> %s\n", s.path.c_str(), hex);
                        written &= writer_->write({"rules", idx, "settings", "color"},
                                                  cfgedit::member(st, "color"), "strip");
                    }
                    if (e.setLevel && s.hasLevel)
                    {
                        s.level = e.level;
                        cfgedit::member(st, "level") = cfgedit::number(s.level);
                        fprintf(stderr, "strip: scene %s level -> %g\n", s.path.c_str(), s.level);
                        written &= writer_->write({"rules", idx, "settings", "level"},
                                                  cfgedit::member(st, "level"), "strip");
                    }
                    changedRules_.push_back(s.rule);
                }
            }

            // the switch itself: the rule's file. Once per path, after the
            // settings, so a color change and "on" in one edit light the new color.
            if (e.on == 1 && !exists(e.path))
            {
                // O_EXCL: never write through something that appeared in the
                // meantime, a symlink least of all — the file's existence is
                // the whole message
                int fd = open(e.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
                if (fd < 0)
                    fprintf(stderr, "strip: can't create %s: %s\n", e.path.c_str(), strerror(errno));
                else
                {
                    close(fd);
                    fprintf(stderr, "strip: scene %s on\n", e.path.c_str());
                }
            }
            else if (e.on == 0 && exists(e.path))
            {
                if (unlink(e.path.c_str()) != 0)
                    fprintf(stderr, "strip: can't remove %s: %s\n", e.path.c_str(), strerror(errno));
                else
                    fprintf(stderr, "strip: scene %s off\n", e.path.c_str());
            }
        }

        return written;
    }

    Config* cfg_ = nullptr;
    cfgedit::Writer* writer_ = nullptr;
    bool hasBlock_ = false;

    int leds_ = 0, pin_ = 0;
    bool reverse_ = false;
    double brightness_ = 1; // a double for the same reason as Scene::level
    float gr_ = 2.2f, gg_ = 2.2f, gb_ = 2.2f;
    std::string gammaText_ = "2.2";
    uint32_t wb_ = 0xFFFFFF;

    std::vector<Scene> scenes_;
    double lastScan_ = -1e9;
    bool stripChanged_ = false;
    std::vector<size_t> changedRules_;
    mutable bool warnedSize_ = false; // toJson is const; the warning is once
};
} // namespace stripcfg
