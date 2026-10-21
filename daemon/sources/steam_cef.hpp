#pragma once

#include <time.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "devtools.hpp"
#include "json.hpp"

// Steam's own state, read from its UI over the remote-debugging port: the
// SteamClient API in the SharedJSContext page is the internal one Decky
// plugins use. One hook follows downloads (steam.hpp), the launch's game
// action (steam_launch.hpp), achievement unlocks (steam_achievements.hpp) and
// the running games; Cef::call runs the phone's requests (steam_remote.hpp).
// Undocumented, so a Steam update can break any of it; downloads fall back
// to the manifests whenever there's no fresh reading.
namespace steam
{

// Steam hardcodes 8080; absent/null/0 = off
inline const char* CONFIG_KEY_DEBUG_PORT = "steam_debug_port";

struct DownloadState
{
    bool running = false;   // underway and not paused
    unsigned appid = 0;     // each part of an install is its own app
    float percent = 0;
    double done = 0;
    double total = 0;
    double rate = 0;        // network bytes/s
    bool paused = false;
    std::string state;      // None / Starting / Updating / Downloading / Stopping
    std::vector<unsigned> completed;
    int queued = 0;         // unfinished items, excluding ones scheduled for later

    bool isCompleted(unsigned app) const
    {
        return std::find(completed.begin(), completed.end(), app) != completed.end();
    }
};

// the game action of a launch underway: Steam's launch dialog
struct LaunchState
{
    unsigned appid = 0;     // 0 = no launch underway
    std::string task;       // ProcessingShaderCache, SynchronizingCloud, CreatingProcess, ...
    double done = 0;        // the task's counts, when it has any
    double total = 0;
};

// the newest unlock since the hook was installed
struct AchievementState
{
    unsigned seq = 0;       // counts unlocks; 0 = none yet
    unsigned appid = 0;
    std::string name;
    float global = -1;      // % of players who have it; < 0 unknown
};

struct RunningApp
{
    unsigned appid = 0;
    std::string name;       // Steam's display name; "" when it doesn't say
};

struct Snapshot
{
    double hook = 0;        // the hook's identity: a new one restarts the counts
    DownloadState downloads;
    LaunchState launch;
    AchievementState achievement;
    std::vector<RunningApp> running;
    std::vector<std::string> missing; // SteamClient functions this Steam lacks
};

namespace cef
{

// installed once per Steam run; the versioned key lets a reconnect or a
// daemon restart reuse the subscriptions. A launch's numbers only exist on
// request (GetActiveGameActions), so they're re-asked on every task change
// and, while a launch runs, on every read.
static const char* INSTALL_JS = R"JS((() => {
  const K = '__ledController2';
  if (window[K]) return 'reused';
  const C = window.SteamClient || {};
  const D = C.Downloads, A = C.Apps, G = C.GameSessions;
  if (!D || typeof D.RegisterForDownloadOverview !== 'function' ||
      typeof D.RegisterForDownloadItems !== 'function')
    return 'SteamClient.Downloads is missing';
  const old = window.__ledControllerDownloads1;
  if (old) {
    for (const u of old.subs || []) try { u.unregister(); } catch (e) {}
    delete window.__ledControllerDownloads1;
  }
  const s = window[K] = { hook: Date.now(), overview: null, items: [], downloading: false,
                          actions: [], achievement: null, unlocks: 0, alive: {}, missing: [], subs: [] };
  const on = (api, fn, cb) => {
    if (api && typeof api[fn] === 'function') s.subs.push(api[fn](cb));
    else s.missing.push(fn);
  };
  s.refreshActions = () => {
    if (A && typeof A.GetActiveGameActions === 'function')
      A.GetActiveGameActions().then(a => { s.actions = Array.isArray(a) ? a : []; }, () => {});
  };
  if (!A || typeof A.GetActiveGameActions !== 'function') s.missing.push('GetActiveGameActions');
  on(D, 'RegisterForDownloadOverview', o => { s.overview = o; });
  on(D, 'RegisterForDownloadItems', (d, items) => { s.downloading = !!d; s.items = items || []; });
  on(A, 'RegisterForGameActionStart', s.refreshActions);
  on(A, 'RegisterForGameActionTaskChange', s.refreshActions);
  on(A, 'RegisterForGameActionEnd', s.refreshActions);
  on(G, 'RegisterForAchievementNotification', n => {
    const max = Number(n && n.nMaxProgress) || 0, cur = Number(n && n.nCurrentProgress) || 0;
    if (max > 0 && cur < max) return;
    const a = (n && n.achievement) || {};
    s.achievement = { seq: ++s.unlocks, appid: Number(n && n.unAppID) || 0,
                      name: String(a.strName || a.strID || ''),
                      global: typeof a.flAchieved === 'number' ? a.flAchieved : -1 };
  });
  on(G, 'RegisterForAppLifetimeNotifications', n => {
    const id = Number(n && n.unAppID) || 0;
    if (!id) return;
    if (n.bRunning) s.alive[id] = 1; else delete s.alive[id];
  });
  s.refreshActions();
  return 'installed';
})())JS";

// Byte counts come as numbers, bigints, strings or protobuf Longs. Older
// clients have flat update_bytes_* fields, current ones a progress[] list
// whose largest entry is the download.
static const char* READ_JS = R"JS((() => {
  const s = window.__ledController2;
  if (!s) return null;
  const num = v => {
    if (typeof v === 'number') return isFinite(v) ? v : 0;
    if (typeof v === 'bigint' || typeof v === 'string') { const n = Number(v); return isFinite(n) ? n : 0; }
    if (v && typeof v === 'object') {
      if (typeof v.toNumber === 'function') return Number(v.toNumber()) || 0;
      if ('low' in v && 'high' in v) return (v.high >>> 0) * 4294967296 + (v.low >>> 0);
    }
    return 0;
  };
  const o = s.overview || {};
  let done = num(o.update_bytes_downloaded), total = num(o.update_bytes_to_download);
  if (!(total > 0) && Array.isArray(o.progress))
    for (const p of o.progress) {
      const t = num(p && (p.bytes_total ?? p.bytesTotal));
      if (t > total) { total = t; done = num(p.bytes_in_progress ?? p.bytesInProgress); }
    }
  const completed = [];
  let queued = 0;
  for (const g of (Array.isArray(s.items) ? s.items : []))
    for (const i of (g && Array.isArray(g.item_data) ? g.item_data : [g]))
      if (i && i.completed) completed.push(num(i.appid));
      else if (i && !(num(i.deferred_time) > 0)) queued++;
  const ui = window.SteamUIStore, apps = ui && ui.RunningApps;
  const running = apps && typeof apps.length === 'number'
    ? Array.from(apps, a => ({ appid: num(a && a.appid), name: String((a && a.display_name) || '') }))
    : Object.keys(s.alive).map(a => ({ appid: Number(a), name: '' }));
  const actions = Array.isArray(s.actions) ? s.actions : [];
  const act = actions.find(a => a && a.strActionName === 'LaunchApp');
  if (actions.length) s.refreshActions();
  return {
    hook: s.hook,
    missing: s.missing,
    downloads: {
      downloading: !!s.downloading,
      completed: completed,
      queued: queued,
      appid: num(o.update_appid ?? o.updateAppId),
      state: String(o.update_state ?? o.updateState ?? 'None'),
      paused: !!o.paused,
      percent: num(o.overall_percent_complete ?? o.overallPercentComplete),
      done: done, total: total,
      rate: num(o.update_network_bytes_per_second ?? o.updateNetworkBytesPerSecond)
    },
    launch: act ? { appid: num(act.gameid), task: String(act.strTaskName || ''),
                    done: num(act.strNumDone), total: num(act.strNumTotal) } : null,
    achievement: s.achievement,
    running: running.filter(a => a.appid > 0)
  };
})())JS";

static const char* DUMP_JS = R"JS((() => {
  const s = window.__ledController2;
  if (!s) return null;
  return JSON.stringify({ downloading: s.downloading, overview: s.overview, items: s.items,
                          actions: s.actions, achievement: s.achievement, missing: s.missing },
                        (k, v) => typeof v === 'bigint' ? v.toString() : v, 1);
})())JS";

inline double num(const json::Value* v) { return v && v->isNumber() ? v->number : 0.0; }

inline std::string str(const json::Value* v) { return v ? json::toString(*v) : ""; }

inline void toDownloads(const json::Value& v, DownloadState& out)
{
    const json::Value* state = v.find("state");
    const json::Value* paused = v.find("paused");
    const json::Value* downloading = v.find("downloading");

    out.state = state ? json::toString(*state) : "None";
    out.paused = paused && json::toBool(*paused);
    out.appid = (unsigned)num(v.find("appid"));
    out.done = num(v.find("done"));
    out.total = num(v.find("total"));
    out.rate = num(v.find("rate"));
    out.queued = (int)num(v.find("queued"));

    out.completed.clear();
    if (const json::Value* c = v.find("completed"))
        for (const auto& a : c->items)
            out.completed.push_back((unsigned)num(&a));

    double pct = num(v.find("percent"));
    if (pct <= 0 && out.total > 0)
        pct = 100.0 * out.done / out.total;
    out.percent = (float)(pct < 0 ? 0 : pct > 100 ? 100 : pct);

    // `downloading` holds across the apps of a queue, where the overview's
    // state may briefly drop to None
    bool working = out.state == "Starting" || out.state == "Updating" ||
                   out.state == "Downloading" || out.state == "Stopping";
    out.running = !out.paused && out.appid > 0 &&
                  ((downloading && json::toBool(*downloading)) || working);
}

inline bool toSnapshot(const json::Value& v, Snapshot& out)
{
    const json::Value* dl = v.isObject() ? v.find("downloads") : nullptr;
    if (!dl || !dl->isObject())
        return false;

    out = Snapshot{};
    out.hook = num(v.find("hook"));
    toDownloads(*dl, out.downloads);

    if (const json::Value* m = v.find("missing"))
        for (const auto& f : m->items)
            out.missing.push_back(json::toString(f));

    const json::Value* l = v.find("launch");
    if (l && l->isObject())
    {
        out.launch.appid = (unsigned)num(l->find("appid"));
        out.launch.task = str(l->find("task"));
        out.launch.done = num(l->find("done"));
        out.launch.total = num(l->find("total"));
    }

    const json::Value* a = v.find("achievement");
    if (a && a->isObject())
    {
        out.achievement.seq = (unsigned)num(a->find("seq"));
        out.achievement.appid = (unsigned)num(a->find("appid"));
        out.achievement.name = str(a->find("name"));
        const json::Value* g = a->find("global");
        out.achievement.global = g && g->isNumber() ? (float)g->number : -1.0f;
    }

    if (const json::Value* r = v.find("running"))
        for (const auto& a : r->items)
            out.running.push_back({(unsigned)num(a.find("appid")), str(a.find("name"))});
    return true;
}

// "" on success; fresh = the hook is new and hasn't had a callback yet
inline std::string attach(devtools::Session& s, int port, bool* fresh = nullptr)
{
    std::string err;
    if (!s.connect(port, "SharedJSContext", err))
        return err + " (is Steam running, with remote debugging on?)";

    json::Value v;
    if (!s.evaluate(INSTALL_JS, v, err))
    {
        s.close();
        return err;
    }

    std::string got = json::toString(v);
    if (got != "installed" && got != "reused")
    {
        s.close();
        return got.empty() ? "couldn't hook into Steam" : got;
    }

    if (fresh)
        *fresh = got == "installed";
    return "";
}

// closes the session on failure
inline bool readSnapshot(devtools::Session& s, Snapshot& out, std::string& err)
{
    json::Value v;

    if (!s.evaluate(READ_JS, v, err))
        err += " (has Steam's JS API changed?)";
    else if (!toSnapshot(v, out))
        err = "Steam's hook is gone";
    else
        return true;

    s.close();
    return false;
}

// The phone's requests (steam_remote.hpp), each an expression answering
// "ok", NOT_READY (Steam is up but hasn't loaded its library yet: a launch
// that early is lost) or why it can't
inline const char* NOT_READY = "not ready";
inline const char* UNREACHABLE = "Steam isn't reachable on its debug port";

inline std::string launchJs(unsigned appid)
{
    return R"JS(((id) => {
  const A = window.SteamClient && SteamClient.Apps;
  if (!A || typeof A.RunGame !== 'function') return 'this Steam lacks SteamClient.Apps.RunGame';
  if (!window.appStore || typeof appStore.GetAppOverviewByAppID !== 'function' ||
      !appStore.GetAppOverviewByAppID(id)) return 'not ready';
  A.RunGame(String(id), '', -1, 100);
  return 'ok';
})()JS" + std::to_string(appid) + ")";
}

inline std::string quitJs(unsigned appid)
{
    return R"JS(((id) => {
  const A = window.SteamClient && SteamClient.Apps;
  if (!A || typeof A.TerminateApp !== 'function') return 'this Steam lacks SteamClient.Apps.TerminateApp';
  A.TerminateApp(String(id), false);
  return 'ok';
})()JS" + std::to_string(appid) + ")";
}

inline std::string downloadsJs(bool enable)
{
    return R"JS(((on) => {
  const D = window.SteamClient && SteamClient.Downloads;
  if (!D || typeof D.EnableAllDownloads !== 'function') return 'this Steam lacks SteamClient.Downloads.EnableAllDownloads';
  D.EnableAllDownloads(on);
  return 'ok';
})()JS" + std::string(enable ? "true" : "false") + ")";
}

} // namespace cef

// polls Steam from a detached thread so the render loop never waits on it
class Cef
{
public:
    static Cef& get()
    {
        static Cef* c = new Cef; // never destroyed: the poller is detached
        return *c;
    }

    void setPort(int port)
    {
        port_.store(port > 0 && port < 65536 ? port : 0);
        cv_.notify_all();
    }

    // false while off, unreachable or stale
    bool read(Snapshot& out)
    {
        if (port_.load() == 0)
            return false;

        want();
        std::lock_guard<std::mutex> g(m_);
        if (mono() - snapAt_ > STALE_S)
            return false;
        out = snap_;
        return true;
    }

    // queue an expression to run in Steam (cef::launchJs, ...) on the
    // poller's session; its answer comes back through result(ticket)
    uint64_t call(std::string js)
    {
        std::lock_guard<std::mutex> g(m_);
        uint64_t t = nextTicket_++;
        if (port_.load() == 0)
            done_[t] = "steam_debug_port is off";
        else
            calls_.push_back({t, std::move(js)});
        want();
        cv_.notify_all();
        return t;
    }

    // true once the call has run (or couldn't): its answer, or why not
    bool result(uint64_t ticket, std::string& out)
    {
        std::lock_guard<std::mutex> g(m_);
        auto it = done_.find(ticket);
        if (it == done_.end())
            return false;
        out = std::move(it->second);
        done_.erase(it);
        return true;
    }

    // one synchronous look, for --steam-status
    static bool probe(int port, Snapshot& snap, std::string& raw, std::string& err)
    {
        devtools::Session s;
        bool fresh = false;

        err = cef::attach(s, port, &fresh);
        if (!err.empty())
            return false;

        if (fresh)
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        if (!cef::readSnapshot(s, snap, err))
            return false;

        json::Value v;
        std::string dumpErr;
        if (s.evaluate(cef::DUMP_JS, v, dumpErr))
            raw = json::toString(v);
        return true;
    }

private:
    static constexpr double WANT_S = 15;    // stop polling when nobody asked for this long
    static constexpr double STALE_S = 3;
    static constexpr double RETRY_S = 10;

    std::atomic<int> port_{0};
    std::atomic<double> wantedAt_{-1e9};
    std::atomic<bool> started_{false};

    std::mutex m_;
    std::condition_variable cv_;
    Snapshot snap_;             // under m_
    double snapAt_ = -1e9;      // under m_

    struct Call
    {
        uint64_t ticket;
        std::string js;
    };
    std::deque<Call> calls_;                // under m_
    std::map<uint64_t, std::string> done_;  // under m_
    uint64_t nextTicket_ = 1;               // under m_

    Cef() = default;

    static double mono()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
    }

    static std::string join(const std::vector<std::string>& v)
    {
        std::string out;
        for (const auto& f : v)
            out += (out.empty() ? "" : ", ") + f;
        return out;
    }

    void want()
    {
        wantedAt_.store(mono());
        if (!started_.exchange(true))
            std::thread(&Cef::loop, this).detach();
    }

    void loop()
    {
        devtools::Session s;
        int tried = 0;
        double lastTry = -1e9;
        std::string logged;

        // journal changes of state only, not every retry
        auto report = [&](int port, const std::string& err) {
            std::string line = err.empty()
                ? "following Steam through its debug port " + std::to_string(port)
                : "debug port " + std::to_string(port) + ": " + err;
            if (line != logged)
                fprintf(stderr, "steam: %s\n", line.c_str());
            logged = line;
        };

        for (;;)
        {
            int port = port_.load();
            double now = mono();
            bool wanted = port > 0 && now - wantedAt_.load() < WANT_S;

            if (!wanted || port != tried)
                s.close();

            if (wanted && !s.open() && (now - lastTry >= RETRY_S || port != tried))
            {
                lastTry = now;
                tried = port;
                std::string err = cef::attach(s, port);
                if (!err.empty())
                    report(port, err);
            }

            if (wanted && s.open())
            {
                Snapshot snap;
                std::string err;

                if (cef::readSnapshot(s, snap, err))
                {
                    if (!snap.missing.empty())
                        err = "following Steam, but this Steam lacks " + join(snap.missing);
                    std::lock_guard<std::mutex> g(m_);
                    snap_ = std::move(snap);
                    snapAt_ = mono();
                }

                report(port, err);
            }

            runCalls(s);

            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::seconds(1), [&] { return !calls_.empty(); });
        }
    }

    // the queued calls, on this session; without one they answer UNREACHABLE
    void runCalls(devtools::Session& s)
    {
        for (;;)
        {
            Call c;
            {
                std::lock_guard<std::mutex> g(m_);
                if (calls_.empty())
                    return;
                c = std::move(calls_.front());
                calls_.pop_front();
            }

            std::string answer = cef::UNREACHABLE;
            if (s.open())
            {
                json::Value v;
                std::string err;
                if (s.evaluate(c.js.c_str(), v, err))
                    answer = json::toString(v);
                else
                    s.close(); // the next pass attaches again
            }

            std::lock_guard<std::mutex> g(m_);
            done_[c.ticket] = std::move(answer);
        }
    }
};

} // namespace steam
