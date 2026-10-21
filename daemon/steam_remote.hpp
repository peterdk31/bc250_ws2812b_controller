#pragma once

#include <stdint.h>
#include <stdio.h>
#include <memory>
#include <string>
#include <vector>

#include "config_edit.hpp"
#include "config_loader.hpp"
#include "protocol.hpp"
#include "sink.hpp"
#include "steam_cef.hpp"
#include "steam_library.hpp"
#include "views.hpp"

// Steam's half of the BLE dashboard (README "BLE remote"): the installed
// games, what Steam is doing, and the phone's requests — launch a game, quit
// one, pause or resume the downloads — run through Steam's debug port
// (steam_cef.hpp Cef::call).
//
// Two views. VIEW_STEAM_LIBRARY is the games from the appmanifests, so it is
// there with Steam closed; the receiver keeps it while the host is off and
// the phone keeps its own copy, which is how a game can be picked before
// boot. VIEW_STEAM is the live state plus the last request and its outcome:
//
//     { "port": true,                       // steam_debug_port is set
//       "steam": true,                      // ...and the daemon is following Steam
//       "game": { "a": 1245620, "n": "ELDEN RING" },          // or null
//       "launch": { "a": 1245620, "task": "ProcessingShaderCache", "p": 42 },
//                                           // or null; "p" only with counts
//       "dl": { "a": 570, "n": "Dota 2", "p": 12, "paused": false, "q": 1 },  // or null
//       "req": { "id": 7, "op": "launch", "a": 1245620,
//                "st": "waiting" | "done" | "failed", "err": "..." } }    // or null
//
//     { "games": [ { "a": 1245620, "n": "ELDEN RING", "t": 1729500000 } ],
//       "more": 0 }                         // games left out for size
//
// An edit is a request: { "id": 7, "op": "launch" | "quit", "a": appid } or
// { "id": 8, "op": "pause" | "resume" }. The id is the phone's: the request
// echoed back in "req" is the answer that it arrived, and the phone resends
// until it sees it — the receiver drops a request while no daemon listens,
// as during a boot the phone itself started. A resend of the request in
// hand only repeats the echo. A launch waits up to LAUNCH_WAIT_S for Steam
// to come up and load its library.
namespace steamremote
{
class Remote
{
public:
    static const uint8_t VIEW = proto::VIEW_STEAM;
    static const uint8_t LIBRARY_VIEW = proto::VIEW_STEAM_LIBRARY;

    void load(const Config& cfg)
    {
        port_ = cfg.getInt(steam::CONFIG_KEY_DEBUG_PORT, 0) > 0;
    }

    void pushAll(std::vector<std::unique_ptr<Sink>>& sinks)
    {
        scanLibrary(sinks, true);
        pushStatus(sinks, true);
    }

    void tick(double now, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        if (now - lastScan_ >= LIBRARY_SCAN_S)
        {
            lastScan_ = now;
            scanLibrary(sinks, false);
        }
        advance(now);
        pushStatus(sinks, false);
    }

    // the phone's request (protocol.hpp MSG_EDIT). Main thread.
    void onEdit(const std::string& edit, std::vector<std::unique_ptr<Sink>>& sinks)
    {
        json::Value v;
        std::string err;
        if (!json::parse(edit, v, err) || !v.isObject())
            return;

        const json::Value* id = v.find("id");
        const json::Value* op = v.find("op");
        const json::Value* app = v.find("a");
        if (!id || !id->isNumber() || !op || !op->isString())
            return;

        Request r;
        r.id = (uint32_t)id->number;
        r.since = progress::mono();
        r.op = op->text;
        r.appid = app && app->isNumber() ? (unsigned)app->number : 0;

        if (r.id != req_.id)
        {
            bool needsApp = r.op == "launch" || r.op == "quit";
            if (r.op != "launch" && r.op != "quit" && r.op != "pause" && r.op != "resume")
                fail(r, "unknown request \"" + r.op + "\"");
            else if (needsApp && !r.appid)
                fail(r, "no game named");
            else if (!port_)
                fail(r, "steam_debug_port is off in the config");
            req_ = r;
            fprintf(stderr, "steam: the phone asks to %s%s\n", r.op.c_str(),
                    r.appid ? (" " + nameOf(r.appid)).c_str() : "");
            advance(r.since);
        }
        pushStatus(sinks, true);
    }

private:
    static constexpr double LIBRARY_SCAN_S = 60;
    static constexpr double LAUNCH_WAIT_S = 300;
    static constexpr double RETRY_S = 2;

    struct Request
    {
        uint32_t id = 0;
        std::string op;
        unsigned appid = 0;
        double since = 0;
        std::string st = "waiting";
        std::string err;
        uint64_t ticket = 0;        // a call into Steam in flight
        double retryAt = 0;
    };

    bool port_ = false;
    Request req_;
    std::vector<steam::Game> games_;
    std::string library_, status_;  // as last sent
    double lastScan_ = -1e9;

    static void fail(Request& r, const std::string& why)
    {
        r.st = "failed";
        r.err = why;
        fprintf(stderr, "steam: the phone's %s failed: %s\n", r.op.c_str(), why.c_str());
    }

    std::string callFor(const Request& r) const
    {
        if (r.op == "launch") return steam::cef::launchJs(r.appid);
        if (r.op == "quit") return steam::cef::quitJs(r.appid);
        return steam::cef::downloadsJs(r.op == "resume");
    }

    // a launch keeps asking until Steam has loaded its library; the rest
    // are asked once
    void advance(double now)
    {
        Request& r = req_;
        if (!r.id || r.st != "waiting")
            return;

        std::string answer;
        if (r.ticket && steam::Cef::get().result(r.ticket, answer))
        {
            r.ticket = 0;
            bool early = answer == steam::cef::NOT_READY || answer == steam::cef::UNREACHABLE;
            if (answer == "ok")
            {
                r.st = "done";
                fprintf(stderr, "steam: %s %s done\n", r.op.c_str(), r.appid ? nameOf(r.appid).c_str() : "");
            }
            else if (r.op == "launch" && early)
                r.retryAt = now + RETRY_S;
            else
                fail(r, answer);
        }

        if (r.st != "waiting" || r.ticket || now < r.retryAt)
            return;
        if (now - r.since > LAUNCH_WAIT_S)
            fail(r, "Steam wasn't ready within " + std::to_string((int)(LAUNCH_WAIT_S / 60)) + " minutes");
        else
            r.ticket = steam::Cef::get().call(callFor(r));
    }

    std::string nameOf(unsigned appid) const
    {
        for (const auto& g : games_)
            if (g.appid == appid)
                return g.name;
        return "app " + std::to_string(appid);
    }

    void scanLibrary(std::vector<std::unique_ptr<Sink>>& sinks, bool force)
    {
        games_ = steam::installedGames();

        std::string list;
        size_t shown = 0;
        for (const auto& g : games_)
        {
            std::string e = "{\"a\":" + std::to_string(g.appid) + ",\"n\":\"" + cfgedit::escape(g.name) +
                            "\",\"t\":" + std::to_string(g.lastPlayed) +
                            (g.art.empty() ? "" : ",\"h\":\"" + cfgedit::escape(g.art) + "\"") + "}";
            // room for the brackets and the "more" count after it
            if (list.size() + e.size() + 40 > proto::VIEW_MAX)
                break;
            list += (shown++ ? "," : "") + e;
        }
        std::string j = "{\"games\":[" + list + "],\"more\":" + std::to_string(games_.size() - shown) + "}";

        if (force || j != library_)
        {
            library_ = j;
            views::send(sinks, LIBRARY_VIEW, j);
        }
    }

    void pushStatus(std::vector<std::unique_ptr<Sink>>& sinks, bool force)
    {
        std::string j = statusJson();
        if (force || j != status_)
        {
            status_ = j;
            views::send(sinks, VIEW, j);
        }
    }

    std::string app(unsigned appid, const std::string& name) const
    {
        return "\"a\":" + std::to_string(appid) + ",\"n\":\"" +
               cfgedit::escape(name.empty() ? nameOf(appid) : name) + "\"";
    }

    std::string statusJson() const
    {
        steam::Snapshot s;
        bool following = port_ && steam::Cef::get().read(s);

        std::string j = std::string("{\"port\":") + (port_ ? "true" : "false") +
                        ",\"steam\":" + (following ? "true" : "false");

        j += ",\"game\":";
        j += following && !s.running.empty() ? "{" + app(s.running[0].appid, s.running[0].name) + "}" : "null";

        j += ",\"launch\":";
        if (following && s.launch.appid)
        {
            j += "{\"a\":" + std::to_string(s.launch.appid) + ",\"task\":\"" + cfgedit::escape(s.launch.task) + "\"";
            if (s.launch.total > 0)
                j += ",\"p\":" + std::to_string((int)(100.0 * s.launch.done / s.launch.total));
            j += "}";
        }
        else
            j += "null";

        const steam::DownloadState& d = s.downloads;
        j += ",\"dl\":";
        if (following && d.appid && (d.running || d.paused || d.queued > 0))
            j += "{" + app(d.appid, "") + ",\"p\":" + std::to_string((int)d.percent) +
                 ",\"paused\":" + (d.paused ? "true" : "false") + ",\"q\":" + std::to_string(d.queued) + "}";
        else
            j += "null";

        j += ",\"req\":";
        if (req_.id)
        {
            j += "{\"id\":" + std::to_string(req_.id) + ",\"op\":\"" + cfgedit::escape(req_.op) +
                 "\",\"a\":" + std::to_string(req_.appid) + ",\"st\":\"" + req_.st + "\"";
            if (!req_.err.empty())
                j += ",\"err\":\"" + cfgedit::escape(req_.err) + "\"";
            j += "}";
        }
        else
            j += "null";

        return j + "}";
    }
};
} // namespace steamremote
