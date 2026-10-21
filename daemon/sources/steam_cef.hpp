#pragma once

#include <time.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "devtools.hpp"
#include "json.hpp"

// Steam's own download numbers, read from its UI over the remote-debugging
// port: SteamClient.Downloads in the SharedJSContext page is the internal
// API Decky plugins use. Undocumented, so a Steam update can break it;
// steam.hpp falls back to the manifests whenever there's no fresh reading.
namespace steam
{

// Steam hardcodes 8080; absent/null/0 = off
inline const char* CONFIG_KEY_DEBUG_PORT = "steam_debug_port";

struct Live
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

namespace cef
{

// installed once per Steam run; the versioned key lets a reconnect or a
// daemon restart reuse the subscriptions
static const char* INSTALL_JS = R"JS((() => {
  const K = '__ledControllerDownloads1';
  if (window[K]) return 'reused';
  const D = window.SteamClient && window.SteamClient.Downloads;
  if (!D || typeof D.RegisterForDownloadOverview !== 'function' ||
      typeof D.RegisterForDownloadItems !== 'function')
    return 'SteamClient.Downloads is missing';
  const s = window[K] = { overview: null, items: [], downloading: false };
  s.subs = [
    D.RegisterForDownloadOverview(o => { s.overview = o; }),
    D.RegisterForDownloadItems((d, items) => { s.downloading = !!d; s.items = items || []; })
  ];
  return 'installed';
})())JS";

// Byte counts come as numbers, bigints or protobuf Longs. Older clients have
// flat update_bytes_* fields, current ones a progress[] list whose largest
// entry is the download.
static const char* READ_JS = R"JS((() => {
  const s = window.__ledControllerDownloads1;
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
  return {
    downloading: !!s.downloading,
    completed: completed,
    queued: queued,
    appid: num(o.update_appid ?? o.updateAppId),
    state: String(o.update_state ?? o.updateState ?? 'None'),
    paused: !!o.paused,
    percent: num(o.overall_percent_complete ?? o.overallPercentComplete),
    done: done, total: total,
    rate: num(o.update_network_bytes_per_second ?? o.updateNetworkBytesPerSecond)
  };
})())JS";

static const char* DUMP_JS = R"JS((() => {
  const s = window.__ledControllerDownloads1;
  if (!s) return null;
  return JSON.stringify({ downloading: s.downloading, overview: s.overview, items: s.items },
                        (k, v) => typeof v === 'bigint' ? v.toString() : v, 1);
})())JS";

inline double num(const json::Value* v) { return v && v->isNumber() ? v->number : 0.0; }

inline bool toLive(const json::Value& v, Live& out)
{
    if (!v.isObject())
        return false;

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
        return got.empty() ? "couldn't hook Steam's downloads" : got;
    }

    if (fresh)
        *fresh = got == "installed";
    return "";
}

// closes the session on failure
inline bool readLive(devtools::Session& s, Live& out, std::string& err)
{
    json::Value v;

    if (!s.evaluate(READ_JS, v, err))
        err += " (has Steam's JS API changed?)";
    else if (!toLive(v, out))
        err = "Steam's download hook is gone";
    else
        return true;

    s.close();
    return false;
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
    bool read(Live& out)
    {
        if (port_.load() == 0)
            return false;

        want();
        std::lock_guard<std::mutex> g(m_);
        if (mono() - liveAt_ > STALE_S)
            return false;
        out = live_;
        return true;
    }

    // one synchronous look, for --steam-status
    static bool probe(int port, Live& live, std::string& raw, std::string& err)
    {
        devtools::Session s;
        bool fresh = false;

        err = cef::attach(s, port, &fresh);
        if (!err.empty())
            return false;

        if (fresh)
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        if (!cef::readLive(s, live, err))
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
    Live live_;                 // under m_
    double liveAt_ = -1e9;      // under m_

    Cef() = default;

    static double mono()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
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
                ? "following downloads through Steam's debug port " + std::to_string(port)
                : "debug port " + std::to_string(port) + ": " + err + " — using the manifests";
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
                Live l;
                std::string err;

                if (cef::readLive(s, l, err))
                {
                    std::lock_guard<std::mutex> g(m_);
                    live_ = l;
                    liveAt_ = mono();
                }

                report(port, err);
            }

            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::seconds(1));
        }
    }
};

} // namespace steam
