#include "fan.hpp"

#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_partition.h"
#include "nvs.h"

#include "dbglog.hpp"
#include "link.hpp"
#include "power_switch.hpp"
#include "protocol.hpp"
#include "util.hpp"

#define FLOG(fmt, ...) dbglog::line("fan: " fmt, ##__VA_ARGS__)

namespace fan
{
// ---- fixed tuning ----

static const uint32_t POLL_MS = 100; // task cadence; plenty for a boost timer
static const uint32_t PWM_HZ = 25000; // the 4-pin fan spec's PWM frequency
static const ledc_timer_bit_t PWM_RES = LEDC_TIMER_11_BIT; // max at 25 kHz off
                                                           // the 80 MHz APB
static const uint32_t PWM_MAX = 1 << 11; // LEDC duty is 0..2^res INCLUSIVE, so
                                         // 100% maps to exactly 2048 (full on)
static const int MAX_FANS = proto::FAN_CHANNELS; // = LEDC channels on the C3
static const uint8_t NONE = proto::FAN_NONE;     // "not set" in every byte slot

// how long the live duties outlive the daemon's last CMD_FAN_LIVE. The daemon
// refreshes them every few seconds even when nothing changed, so this only
// ever fires when the daemon is really gone (crash, kill, unplug) — and then
// the fallback duty takes over
static const uint32_t LIVE_TIMEOUT_MS = 15000;

// how long the USB SOF keepalives must be gone before "present again" counts
// as the host coming back up — the same window led_service uses: bus resets
// during re-enumeration silence SOF for ~1 s, and those are not power cycles
static const uint32_t HOST_GONE_MS = 3000;

// ---- the standalone settings (fancfg, NVS, CMD_FAN_STANDALONE) ----

// One shape on every side: what a header runs when the daemon isn't driving
// it. Its wire form (CMD_FAN_STANDALONE's payload, and the NVS blob) is
// boostSecs(1) then per header duty(1) boost(1).
struct Standalone
{
    uint8_t boostSecs = 5;
    uint8_t duty[MAX_FANS];  // resting duty percent per header
    uint8_t boost[MAX_FANS]; // boost duty percent, NONE = sits the boost out

    Standalone()
    {
        memset(duty, 100, sizeof duty); // full is the safe cooling answer
        memset(boost, NONE, sizeof boost);
    }

    static const uint16_t WIRE_LEN = 1 + 2 * MAX_FANS;

    void encode(uint8_t* p) const
    {
        p[0] = boostSecs;
        for (int i = 0; i < MAX_FANS; i++)
        {
            p[1 + 2 * i] = duty[i];
            p[2 + 2 * i] = boost[i];
        }
    }

    // merge a wire blob in: a header whose duty is NONE is "not the daemon's
    // to drive" and keeps what it has — both fields
    void merge(const uint8_t* p)
    {
        boostSecs = p[0];
        for (int i = 0; i < MAX_FANS; i++)
        {
            if (p[1 + 2 * i] == NONE)
                continue;
            duty[i] = p[1 + 2 * i] > 100 ? 100 : p[1 + 2 * i];
            uint8_t b = p[2 + 2 * i];
            boost[i] = b == NONE ? NONE : b > 100 ? 100 : b;
        }
    }

    bool operator==(const Standalone& o) const
    {
        return boostSecs == o.boostSecs &&
               memcmp(duty, o.duty, sizeof duty) == 0 &&
               memcmp(boost, o.boost, sizeof boost) == 0;
    }
};

// ---- configuration (the `fancfg` flash partition) ----

// Same scheme as the power switch's `pwrcfg` (see power_switch.cpp): wiring
// in its own 4 KB partition, written at flash time by `make flash` /
// `make flash-fan` from the daemon config's "fans" block (tools/fancfg.py
// encodes it, and must match decode() below). Two layouts are understood:
//
//     "FAN2" magic, then
//     enabled(1) boost_secs(1) pin[6] duty[6] boost[6]
//
//     "FAN1" magic (the original), then
//     enabled(1) pin[6] duty[6] boost_duty(1) boost_secs(1)
//
// Slot i is header i+1. Pins are GPIO numbers, 0xFF = header not wired (a
// disabled header). Duties are percent, >100 clamps to 100 — so erased flash
// in an appended field's place reads as a sane full-speed value. boost 0xFF =
// that header sits the boost out; FAN1's single boost_duty applied to every
// header. An erased partition (no magic) leaves the feature off.

static const uint16_t WIRE_LEN_V1 = 15;
static const uint16_t WIRE_LEN_V2 = 20;

struct Config
{
    bool enabled = false;
    int8_t pin[MAX_FANS] = {-1, -1, -1, -1, -1, -1};
    Standalone sa;

    bool decode(const uint8_t* b, uint16_t len)
    {
        auto pinOf = [](uint8_t v) -> int8_t
        { return (v == 0xFF || v >= GPIO_NUM_MAX) ? -1 : (int8_t)v; };
        auto pct = [](uint8_t v) -> uint8_t { return v > 100 ? 100 : v; };

        if (memcmp(b, "FAN2", 4) == 0 && len >= 4 + WIRE_LEN_V2)
        {
            const uint8_t* p = b + 4;
            enabled = p[0] != 0;
            sa.boostSecs = p[1];
            for (int i = 0; i < MAX_FANS; i++)
            {
                pin[i] = pinOf(p[2 + i]);
                sa.duty[i] = pct(p[8 + i]);
                sa.boost[i] = p[14 + i] == NONE ? NONE : pct(p[14 + i]);
            }
            return true;
        }

        if (memcmp(b, "FAN1", 4) == 0 && len >= 4 + WIRE_LEN_V1)
        {
            const uint8_t* p = b + 4;
            enabled = p[0] != 0;
            for (int i = 0; i < MAX_FANS; i++)
            {
                pin[i] = pinOf(p[1 + i]);
                sa.duty[i] = pct(p[7 + i]);
                sa.boost[i] = pct(p[13]);
            }
            sa.boostSecs = p[14];
            return true;
        }

        return false;
    }
};

static Config g_cfg; // loaded once in start(), read-only after

// loadConfig returns false only when the partition itself is missing — the
// one case worth a log line, since it means the chip's partition table
// predates the feature and a `make flash-fan` would land in dead space.
// An erased or disabled config is the normal opted-out state and stays quiet.
static bool loadConfig(Config& c, bool& partitionFound)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fancfg");
    partitionFound = part != nullptr;
    if (!part)
        return false;

    uint8_t b[4 + WIRE_LEN_V2];
    if (esp_partition_read(part, 0, b, sizeof b) != ESP_OK)
        return false;

    return c.decode(b, sizeof b);
}

// ---- state ----

static nvs_handle_t g_nvs = 0;
static bool g_wired[MAX_FANS];  // header i has a pin and an LEDC channel (= i)
static Standalone g_sa;         // standalone settings in force (flash
                                // defaults, overridden by a persisted push)
static uint8_t g_live[MAX_FANS]; // the daemon's live duties, NONE = not driven
static uint32_t g_liveMs = 0;    // millis() of the last live push
static bool g_hold = false;      // host announced shutdown: live never expires
static bool g_started = false;   // gate for the setters; set before the led_rx
                                 // task exists, so never raced

// host pushes in flight from the led_rx task to this one — each whole blob
// moves under one short critical section, the hostreq pattern
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_pendSa[Standalone::WIRE_LEN];
static bool g_pendSaSet = false;
static uint8_t g_pendLive[MAX_FANS];
static bool g_pendLiveSet = false;
static bool g_pendShutdown = false;

// the daemon's two dashboard payloads (fan.hpp, "the BLE dashboard's view"),
// kept verbatim under their own lock — the fan task never reads them, so
// they can't contend with its pending-push section
static portMUX_TYPE g_dashMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_hostCfg[HOST_CONFIG_MAX];
static uint16_t g_hostCfgLen = 0;
static uint32_t g_hostCfgSeq = 0;
static uint8_t g_hostTel[HOST_TELEM_MAX];
static uint16_t g_hostTelLen = 0;
static uint32_t g_hostTelSeq = 0;
static uint32_t g_hostTelMs = 0;

// boost state (see boostCheck): with the power switch present, an armed
// one-shot keyed to its power-on events; without it, an edge detector on USB
// SOF presence (which the UART build hardcodes true, so the boost fires once
// at task start there)
static bool g_boosting = false;
static uint32_t g_boostStart = 0;
static uint32_t g_seenPowerOn = 0; // last pwr::powerOnSeq() acted on
static bool g_armed = false;       // a power-on happened; boost once sense is up
static bool g_railWasUp = false;   // USB path only
static uint32_t g_usbSilentSince = 0; // millis() when SOF stopped (0 = present)

// ---- LEDC ----

static uint32_t dutyOf(uint8_t pct)
{
    return ((uint32_t)pct * PWM_MAX + 50) / 100;
}

// the duty header i runs right now: boost, then live, then resting
static uint8_t effective(int i)
{
    if (g_boosting && g_sa.boost[i] != NONE)
        return g_sa.boost[i];
    if (g_live[i] != NONE)
        return g_live[i];
    return g_sa.duty[i];
}

static void applyAll()
{
    for (int i = 0; i < MAX_FANS; i++)
    {
        if (!g_wired[i])
            continue;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i,
                      dutyOf(effective(i)));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
}

// "65/-/40/-/-/-" style for the log — a dash for a header that isn't wired
// (or, for live/boost, isn't set)
static const char* fmtDuties(const uint8_t* d, char* buf, size_t n,
                             bool onlyWired = true)
{
    size_t at = 0;
    for (int i = 0; i < MAX_FANS && at + 5 < n; i++)
    {
        if ((onlyWired && !g_wired[i]) || d[i] == NONE)
            at += snprintf(buf + at, n - at, "%s-", i ? "/" : "");
        else
            at += snprintf(buf + at, n - at, "%s%u", i ? "/" : "", d[i]);
    }
    return buf;
}

// ---- NVS ----

// The host-pushed standalone settings persist so a daemon-less boot still
// runs the configured fallback and boost. Alongside them, the flash defaults
// they were saved under: a re-flashed fancfg with different values is the
// user re-deciding, and outranks a stale push — the same newer-default-wins
// rule the LED service applies to its saved baud.
static void loadSaved()
{
    uint8_t saved[Standalone::WIRE_LEN], base[Standalone::WIRE_LEN];
    size_t n = sizeof saved;
    if (nvs_get_blob(g_nvs, "sa", saved, &n) != ESP_OK || n != sizeof saved)
        return;
    n = sizeof base;
    if (nvs_get_blob(g_nvs, "sabase", base, &n) != ESP_OK || n != sizeof base)
        return;

    uint8_t cur[Standalone::WIRE_LEN];
    g_cfg.sa.encode(cur);
    if (memcmp(base, cur, sizeof cur) != 0)
        return; // fancfg was re-flashed with new values since this was saved

    Standalone s = g_cfg.sa;
    s.merge(saved);
    g_sa = s;
}

static void persist()
{
    uint8_t sa[Standalone::WIRE_LEN], base[Standalone::WIRE_LEN];
    g_sa.encode(sa);
    g_cfg.sa.encode(base);
    nvs_set_blob(g_nvs, "sa", sa, sizeof sa);
    nvs_set_blob(g_nvs, "sabase", base, sizeof base);
    nvs_commit(g_nvs);
}

// ---- the task ----

// drain whatever the host pushed since the last tick. Standalone settings
// persist (writes only on change, so a daemon restarting with the same config
// costs no flash wear); live duties never do
static void drainPending(uint32_t now)
{
    uint8_t sa[Standalone::WIRE_LEN], live[MAX_FANS];
    bool saSet, liveSet, shutdown;

    taskENTER_CRITICAL(&g_mux);
    saSet = g_pendSaSet;
    liveSet = g_pendLiveSet;
    shutdown = g_pendShutdown;
    if (saSet)
        memcpy(sa, g_pendSa, sizeof sa);
    if (liveSet)
        memcpy(live, g_pendLive, sizeof live);
    g_pendSaSet = g_pendLiveSet = g_pendShutdown = false;
    taskEXIT_CRITICAL(&g_mux);

    bool changed = false;
    char b1[40], b2[40];

    if (saSet)
    {
        Standalone s = g_sa;
        s.merge(sa);
        if (!(s == g_sa))
        {
            g_sa = s;
            persist();
            FLOG("host set fallback %s, boost %s for %us",
                 fmtDuties(g_sa.duty, b1, sizeof b1),
                 fmtDuties(g_sa.boost, b2, sizeof b2), g_sa.boostSecs);
            changed = true;
        }
    }

    if (liveSet)
    {
        // the daemon is (back) in charge: a fresh push ends any shutdown hold
        g_hold = false;
        g_liveMs = now ? now : 1;
        if (memcmp(live, g_live, sizeof live) != 0)
        {
            // one line per change, but not one per degree: a curve creeping
            // up a hill would otherwise flood the ring buffer
            static uint32_t lastLog = 0;
            memcpy(g_live, live, sizeof live);
            if (!lastLog || now - lastLog > 10000)
            {
                lastLog = now;
                FLOG("live %s", fmtDuties(g_live, b1, sizeof b1));
            }
            changed = true;
        }
    }

    if (shutdown && !g_hold)
    {
        g_hold = true;
        FLOG("host shutting down — holding live %s",
             fmtDuties(g_live, b1, sizeof b1));
    }

    if (changed)
        applyAll();
}

// live duties are only as good as the daemon that keeps sending them
static void liveCheck(uint32_t now)
{
    if (g_hold || !g_liveMs || now - g_liveMs <= LIVE_TIMEOUT_MS)
        return;

    bool any = false;
    for (int i = 0; i < MAX_FANS; i++)
        any |= g_live[i] != NONE;

    g_liveMs = 0;
    if (!any)
        return;

    char b1[40];
    memset(g_live, NONE, sizeof g_live);
    FLOG("host silent %us — live duties dropped, fallback %s",
         (unsigned)(LIVE_TIMEOUT_MS / 1000), fmtDuties(g_sa.duty, b1, sizeof b1));
    applyAll();
}

// the boost trigger. With the power switch live (senseState() >= 0) the boost
// is tied to an actual power-on: pwr::powerOnSeq() ticks when the switch
// asserts PS_ON# from a button press — never when this chip merely restarts
// under a running machine — which ARMS the boost, and it fires once the sense
// wire confirms the rail up. Keying off the event instead of the rail's edge
// is what keeps a warm reset of this chip (a crash, a reflash, whatever a
// daemon reconnect provokes) from re-firing it: after any reset the sense
// line reads down for one debounce before coming back up, and an edge
// detector can't tell that settling from a real power-on — it boosted the
// pump on every daemon restart. (A fan that loses its PWM during the reset
// itself briefly runs full per the 4-pin spec, so the pump stays primed
// through resets regardless — the deliberate crash-reprime this replaces was
// redundant.)
//
// Without the power switch, USB SOF presence stands in as before: an edge
// detector debounced HOST_GONE_MS on the way down so a ~1 s re-enumeration
// blip can't read as a power cycle; it starts "down", so a cold boot with the
// host already running boosts once at task start (and the UART build, which
// hardcodes hostPresent() true, fires exactly once there).
//
// A power-on is also a fresh power cycle for the live duties: whatever the
// last daemon left behind (a shutdown hold, most likely) is cleared, and the
// headers run their fallback until the new daemon pushes.
static void boostCheck(uint32_t now)
{
    bool fire;
    int s = pwr::senseState();

    if (s >= 0)
    {
        uint32_t seq = pwr::powerOnSeq();
        if (seq != g_seenPowerOn) // != not >: the counter may wrap, ours resets
        {
            g_seenPowerOn = seq;
            g_armed = true;
        }

        fire = g_armed && s == 1;
        if (fire)
            g_armed = false;
    }
    else
    {
        bool up;
        if (link::hostPresent())
        {
            g_usbSilentSince = 0;
            up = true;
        }
        else
        {
            if (!g_usbSilentSince)
                g_usbSilentSince = now ? now : 1;
            // hold the previous reading through a short blip
            up = now - g_usbSilentSince <= HOST_GONE_MS ? g_railWasUp : false;
        }

        fire = up && !g_railWasUp;
        g_railWasUp = up;
    }

    if (fire)
    {
        bool anyBoost = false;
        for (int i = 0; i < MAX_FANS; i++)
            anyBoost |= g_wired[i] && g_sa.boost[i] != NONE;

        memset(g_live, NONE, sizeof g_live);
        g_liveMs = 0;
        g_hold = false;

        char b1[40];
        if (anyBoost && g_sa.boostSecs)
        {
            g_boosting = true;
            g_boostStart = now;
            FLOG("boost: host powered on — %s for %us (source=%s)",
                 fmtDuties(g_sa.boost, b1, sizeof b1), g_sa.boostSecs,
                 s >= 0 ? "sense" : "usb");
        }
        applyAll();
    }

    if (g_boosting && now - g_boostStart >= (uint32_t)g_sa.boostSecs * 1000)
    {
        char b1[40];
        g_boosting = false;
        FLOG("boost done — settling to %s", fmtDuties(g_sa.duty, b1, sizeof b1));
        applyAll();
    }
}

static void taskMain(void*)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        uint32_t now = millis();
        drainPending(now);
        liveCheck(now);
        boostCheck(now);
    }
}

// ---- public API ----

void setStandalone(const uint8_t* payload, uint16_t len)
{
    if (!g_started || len < Standalone::WIRE_LEN)
        return;

    taskENTER_CRITICAL(&g_mux);
    memcpy(g_pendSa, payload, Standalone::WIRE_LEN);
    g_pendSaSet = true;
    taskEXIT_CRITICAL(&g_mux);
}

void setLive(const uint8_t* payload, uint16_t len)
{
    if (!g_started || len < MAX_FANS)
        return;

    taskENTER_CRITICAL(&g_mux);
    for (int i = 0; i < MAX_FANS; i++)
        g_pendLive[i] = payload[i] == NONE ? NONE
                        : payload[i] > 100 ? 100 : payload[i];
    g_pendLiveSet = true;
    taskEXIT_CRITICAL(&g_mux);
}

void hostShutdown()
{
    if (!g_started)
        return;

    taskENTER_CRITICAL(&g_mux);
    g_pendShutdown = true;
    taskEXIT_CRITICAL(&g_mux);
}

// ---- the dashboard's view ----

void setHostConfig(const uint8_t* payload, uint16_t len)
{
    // a payload this build can't hold is a newer daemon's; drop it rather
    // than serve a truncated one the phone would misread
    if (len == 0 || len > sizeof g_hostCfg)
        return;

    taskENTER_CRITICAL(&g_dashMux);
    memcpy(g_hostCfg, payload, len);
    g_hostCfgLen = len;
    g_hostCfgSeq++;
    taskEXIT_CRITICAL(&g_dashMux);
}

void setHostTelemetry(const uint8_t* payload, uint16_t len)
{
    if (len == 0 || len > sizeof g_hostTel)
        return;

    uint32_t now = millis();
    taskENTER_CRITICAL(&g_dashMux);
    memcpy(g_hostTel, payload, len);
    g_hostTelLen = len;
    g_hostTelSeq++;
    g_hostTelMs = now ? now : 1;
    taskEXIT_CRITICAL(&g_dashMux);
}

uint16_t hostConfig(uint8_t* out, uint16_t max, uint32_t* seq)
{
    taskENTER_CRITICAL(&g_dashMux);
    uint16_t n = g_hostCfgLen <= max ? g_hostCfgLen : 0;
    if (n)
        memcpy(out, g_hostCfg, n);
    if (seq)
        *seq = g_hostCfgSeq;
    taskEXIT_CRITICAL(&g_dashMux);
    return n;
}

uint16_t hostTelemetry(uint8_t* out, uint16_t max, uint32_t* seq, uint32_t* ageMs)
{
    uint32_t now = millis();
    taskENTER_CRITICAL(&g_dashMux);
    uint16_t n = g_hostTelLen <= max ? g_hostTelLen : 0;
    if (n)
        memcpy(out, g_hostTel, n);
    if (seq)
        *seq = g_hostTelSeq;
    if (ageMs)
        *ageMs = g_hostTelMs ? now - g_hostTelMs : 0xFFFFFFFFu;
    taskEXIT_CRITICAL(&g_dashMux);
    return n;
}

void snapshot(Snapshot& s)
{
    // the fan task's own state, read as it stands: every field is a byte the
    // fan task writes without a lock, so this is a best-effort view — a read
    // that straddles a tick can pair one header's new duty with another's old
    // one for one dashboard poll, which is all the view is for
    s.active = g_started;
    s.boosting = g_boosting;
    s.hold = g_hold;
    s.live = false;
    for (int i = 0; i < MAX_FANS; i++)
    {
        s.wired[i] = g_started && g_wired[i];
        if (!s.wired[i])
        {
            s.duty[i] = NONE;
            s.source[i] = SRC_NONE;
            continue;
        }
        s.duty[i] = effective(i);
        if (g_boosting && g_sa.boost[i] != NONE)
            s.source[i] = SRC_BOOST;
        else if (g_live[i] != NONE)
        {
            s.source[i] = SRC_LIVE;
            s.live = true;
        }
        else
            s.source[i] = SRC_FALLBACK;
    }
}

void start()
{
    bool partitionFound = false;
    bool loaded = loadConfig(g_cfg, partitionFound);

    if (!partitionFound)
    {
        // the guardrail for a `make flash-fan` against an older layout: the
        // blob lands in what that table thinks is the factory tail, and the
        // feature silently never exists. Say why.
        FLOG("no fancfg partition — this chip's partition table predates the "
             "fan feature; reflash the firmware (make flash / flash-source)");
        return;
    }

    if (!loaded || !g_cfg.enabled)
        return; // erased or switched off: the normal opted-out state

    int count = 0;
    for (int i = 0; i < MAX_FANS; i++)
    {
        g_wired[i] = g_cfg.pin[i] >= 0;
        count += g_wired[i];
    }

    if (count == 0)
    {
        FLOG("enabled but no header wired — feature off");
        return;
    }

    memset(g_live, NONE, sizeof g_live);
    g_sa = g_cfg.sa;

    nvs_open("fan", NVS_READWRITE, &g_nvs);
    loadSaved();

    // one timer at the fan frequency, one channel per wired header (header
    // i+1 = LEDC channel i). All LEDC on the C3 is the one low-speed group;
    // the strip's SPI (or RMT) is a different peripheral entirely, so the two
    // never contend.
    ledc_timer_config_t tc = {};
    tc.speed_mode = LEDC_LOW_SPEED_MODE;
    tc.duty_resolution = PWM_RES;
    tc.timer_num = LEDC_TIMER_0;
    tc.freq_hz = PWM_HZ;
    tc.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&tc) != ESP_OK)
    {
        FLOG("LEDC timer init FAILED — feature off");
        return;
    }

    for (int i = 0; i < MAX_FANS; i++)
    {
        if (!g_wired[i])
            continue;

        ledc_channel_config_t cc = {};
        cc.gpio_num = g_cfg.pin[i];
        cc.speed_mode = LEDC_LOW_SPEED_MODE;
        cc.channel = (ledc_channel_t)i;
        cc.timer_sel = LEDC_TIMER_0;
        cc.duty = dutyOf(g_sa.duty[i]);
        cc.hpoint = 0;
        if (ledc_channel_config(&cc) != ESP_OK)
        {
            FLOG("header%d (gpio%d) init FAILED", i + 1, g_cfg.pin[i]);
            g_wired[i] = false;
        }
    }

    char b1[40], b2[40], b3[40];
    uint8_t pins[MAX_FANS];
    for (int i = 0; i < MAX_FANS; i++)
        pins[i] = g_cfg.pin[i] < 0 ? NONE : (uint8_t)g_cfg.pin[i];
    FLOG("cfg %d headers, pins %s, fallback %s, boost %s for %us", count,
         fmtDuties(pins, b1, sizeof b1, false), fmtDuties(g_sa.duty, b2, sizeof b2),
         fmtDuties(g_sa.boost, b3, sizeof b3), g_sa.boostSecs);

    g_started = true;

    // priority 2 like the power switch: a 100 ms duty tick never needs to win
    // against the LED service's latch cadence
    xTaskCreate(taskMain, "fan", 4096, nullptr, 2, nullptr);
}
} // namespace fan
