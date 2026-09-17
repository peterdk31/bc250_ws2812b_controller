#include "fan.hpp"

#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"

#include "cfgstore.hpp"
#include "dbglog.hpp"
#include "fancurve.hpp"
#include "link.hpp"
#include "power_switch.hpp"
#include "protocol.hpp"
#include "render.hpp"
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
static const int POINTS = proto::FAN_CURVE_POINTS;
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

// the gpio sources' sampler (see sampleInputs): how often a reading is
// taken, and how long each one looks at the pins. A PC fan PWM is 21..28 kHz
// (a 36..48 µs period), so a 2 ms window averages some fifty periods; the
// input is read as a register, so one window covers every input pin at once
static const uint32_t IN_PERIOD_MS = 250;
static const int64_t IN_WINDOW_US = 2000;

// ---- the standalone settings (fancfg, NVS, CMD_FAN_STANDALONE) ----

// One shape on every side: what a header runs when the daemon isn't driving
// it, and which headers this board runs a curve for itself. Its wire form
// (CMD_FAN_STANDALONE's payload, the NVS blob, the tail of the fancfg blob —
// see protocol.hpp) is
//
//     boostSecs(1) then per header duty(1) boost(1)                 (V1_LEN)
//     ramp(1) then per header kind(1) gpio(1) npts(1) pts[POINTS][2]
struct Standalone
{
    uint8_t boostSecs = 5;
    uint8_t duty[MAX_FANS];  // resting duty percent per header
    uint8_t boost[MAX_FANS]; // boost duty percent, NONE = sits the boost out
    uint8_t ramp = 5;        // slow-down rate, whole percent per second (0 = instant)
    uint8_t kind[MAX_FANS];  // proto::FAN_KIND_*
    uint8_t gpio[MAX_FANS];  // a gpio header's input pin, else NONE
    uint8_t npts[MAX_FANS];  // a gpio header's curve: points (0 = none)
    uint8_t pts[MAX_FANS][POINTS][2]; // (input %, duty %), sorted by input

    static const uint16_t WIRE_LEN = proto::FAN_STANDALONE_LEN;
    static const uint16_t V1_LEN = proto::FAN_STANDALONE_V1_LEN;
    static const uint16_t PER_HEADER = 3 + 2 * POINTS;

    Standalone()
    {
        memset(duty, 100, sizeof duty); // full is the safe cooling answer
        memset(boost, NONE, sizeof boost);
        memset(kind, proto::FAN_KIND_HOST, sizeof kind);
        memset(gpio, NONE, sizeof gpio);
        memset(npts, 0, sizeof npts);
        memset(pts, 0, sizeof pts);
    }

    void encode(uint8_t* p) const
    {
        p[0] = boostSecs;
        for (int i = 0; i < MAX_FANS; i++)
        {
            p[1 + 2 * i] = duty[i];
            p[2 + 2 * i] = boost[i];
        }
        p[V1_LEN] = ramp;
        for (int i = 0; i < MAX_FANS; i++)
        {
            uint8_t* q = p + V1_LEN + 1 + i * PER_HEADER;
            q[0] = kind[i];
            q[1] = gpio[i];
            q[2] = npts[i];
            memcpy(q + 3, pts[i], 2 * POINTS);
        }
    }

    // a curve as it arrives, checked and clamped: 1..POINTS points, inputs
    // strictly rising, duties 0..100. False = keep what was there
    static bool curveOk(uint8_t n, const uint8_t* p)
    {
        if (n < 1 || n > POINTS)
            return false;
        for (int j = 0; j < n; j++)
        {
            if (p[2 * j] > 100 || p[2 * j + 1] > 100)
                return false;
            if (j && p[2 * j] <= p[2 * j - 2])
                return false;
        }
        return true;
    }

    // merge a wire blob in: `len` is V1_LEN (an older daemon: duties and
    // boosts only) or WIRE_LEN. A header whose duty is NONE is "not the
    // daemon's to drive" and keeps what it has — every field; a kind of NONE
    // keeps the source part
    void merge(const uint8_t* p, uint16_t len)
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

        if (len < WIRE_LEN)
            return;

        if (p[V1_LEN] != NONE)
            ramp = p[V1_LEN];
        for (int i = 0; i < MAX_FANS; i++)
        {
            const uint8_t* q = p + V1_LEN + 1 + i * PER_HEADER;
            if (p[1 + 2 * i] == NONE || q[0] == NONE)
                continue;
            setSource(i, q[0], q[1], q[2], q + 3);
        }
    }

    // one header's source part; a bad curve leaves the header on its fallback
    void setSource(int i, uint8_t k, uint8_t g, uint8_t n, const uint8_t* p)
    {
        kind[i] = k > proto::FAN_KIND_HOST ? proto::FAN_KIND_HOST : k;
        gpio[i] = kind[i] == proto::FAN_KIND_GPIO ? g : NONE;
        npts[i] = 0;
        memset(pts[i], 0, sizeof pts[i]);
        if (kind[i] == proto::FAN_KIND_GPIO && curveOk(n, p))
        {
            npts[i] = n;
            memcpy(pts[i], p, 2 * n);
        }
    }

    bool operator==(const Standalone& o) const
    {
        uint8_t a[WIRE_LEN], b[WIRE_LEN];
        encode(a);
        o.encode(b);
        return memcmp(a, b, WIRE_LEN) == 0;
    }
};

// ---- configuration (the `fancfg` flash partition) ----

// Same scheme as the power switch's `pwrcfg` (see power_switch.cpp): wiring
// in its own 4 KB partition, written at flash time by `make flash` /
// `make flash-fan` from the daemon config's "fans" block (tools/fancfg.py
// encodes it, and must match decode() below). Three layouts are understood:
//
//     "FAN3" magic, then
//     enabled(1) boost_secs(1) pin[6] duty[6] boost[6]
//     ramp(1), then per header kind(1) gpio(1) npts(1) pts[POINTS][2]
//
//     "FAN2" magic: the first line of FAN3 alone
//
//     "FAN1" magic (the original), then
//     enabled(1) pin[6] duty[6] boost_duty(1) boost_secs(1)
//
// Slot i is header i+1. Pins are GPIO numbers, 0xFF = header not wired (a
// header the config doesn't list). Duties are percent, >100 clamps to 100 —
// so erased flash in an appended field's place reads as a sane full-speed
// value. boost 0xFF = that header sits the boost out; FAN1's single
// boost_duty applied to every header. FAN3's tail is the Standalone wire
// form's second part: a kind of 0xFF (erased) means host — the header runs
// its fallback until the daemon says otherwise, exactly what FAN2 meant. An
// erased partition (no magic) leaves the feature off.

static const uint16_t BODY_LEN_V1 = 15;
static const uint16_t BODY_LEN_V2 = 20;
static const uint16_t BODY_LEN_V3 = BODY_LEN_V2 + 1 + MAX_FANS * Standalone::PER_HEADER;

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

        bool v3 = memcmp(b, "FAN3", 4) == 0;
        if ((v3 || memcmp(b, "FAN2", 4) == 0) && len >= 4 + BODY_LEN_V2)
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

            if (v3 && len >= 4 + BODY_LEN_V3)
            {
                const uint8_t* t = p + BODY_LEN_V2;
                if (t[0] != NONE)
                    sa.ramp = t[0];
                for (int i = 0; i < MAX_FANS; i++)
                {
                    const uint8_t* q = t + 1 + i * Standalone::PER_HEADER;
                    if (q[0] != NONE)
                        sa.setSource(i, q[0], q[1], q[2], q + 3);
                }
            }
            return true;
        }

        if (memcmp(b, "FAN1", 4) == 0 && len >= 4 + BODY_LEN_V1)
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
    uint8_t b[4 + BODY_LEN_V3];
    if (!cfgstore::partition("fancfg", b, sizeof b, &partitionFound))
        return false;

    return c.decode(b, sizeof b);
}

// ---- state ----

static nvs_handle_t g_nvs = 0;
static bool g_wired[MAX_FANS];  // header i has a pin and an LEDC channel (= i)
static Standalone g_sa;         // standalone settings in force (flash
                                // defaults, overridden by a persisted push)
static volatile uint32_t g_saSeq = 1; // bumps when g_sa changes (standalone())
static uint8_t g_live[MAX_FANS]; // the daemon's live duties, NONE = not driven
static uint32_t g_liveMs = 0;    // millis() of the last live push
static bool g_hold = false;      // host announced shutdown: live never expires
static bool g_started = false;   // gate for the setters; set before the led_rx
                                 // task exists, so never raced

// the gpio sources: the pin each header samples (-1 = none: not a gpio
// header, or a pin this board can't read), the last reading, and the curve's
// ramped output
static int8_t g_inPin[MAX_FANS];
static uint8_t g_in[MAX_FANS];    // input percent, NONE = no reading
static uint8_t g_curve[MAX_FANS]; // the curve's duty, NONE = none in force
static float g_curveF[MAX_FANS];  // ...and its ramp state
static bool g_curveHave[MAX_FANS];
static uint32_t g_lastSample = 0;
static uint8_t g_applied[MAX_FANS]; // the duty last written to each channel

// host pushes in flight from the led_rx task to this one — each whole blob
// moves under one short critical section, the hostreq pattern
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_pendSa[Standalone::WIRE_LEN];
static uint16_t g_pendSaLen = 0; // 0 = none pending
static uint8_t g_pendLive[MAX_FANS];
static bool g_pendLiveSet = false;
static bool g_pendShutdown = false;
static uint8_t g_pendFb[MAX_FANS]; // the phone's per-header fallback edits,
                                   // NONE = none pending for that header
struct PendSource                  // the phone's per-header source edits
{
    bool set;
    uint8_t kind, gpio, npts;
    uint8_t pts[2 * POINTS];
};
static PendSource g_pendSrc[MAX_FANS];

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

// the duty header i runs right now: boost, then this board's own curve,
// then the daemon's live duty, then resting
static uint8_t effective(int i)
{
    if (g_boosting && g_sa.boost[i] != NONE)
        return g_sa.boost[i];
    if (g_curve[i] != NONE)
        return g_curve[i];
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
        uint8_t d = effective(i);
        if (d == g_applied[i])
            continue;
        g_applied[i] = d;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, dutyOf(d));
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

// ---- the gpio sources ----

// can this board read a PWM on GPIO g? The flasher checks a configured pin
// against the same facts (tools/fancfg.py, pincheck.py); this is the check
// for a pin that arrives at runtime — a daemon push, a phone edit — and the
// one place the phone's edit can be refused with a reason. Pins that would
// hurt: another feature's, one of our own outputs, the flash pads, the host
// link, and the boot straps (a PWM at 0 % is a pin held low at reset).
static bool inputPinFree(int g, const char** why)
{
    const char* r = nullptr;
    if (g < 0 || g >= GPIO_NUM_MAX || !GPIO_IS_VALID_GPIO(g))
        r = "not a GPIO on this chip";
    else if (pwr::usesPin(g))
        r = "the power switch's pin";
    else if (render::up() && render::pin() == g)
        r = "the strip's data pin"; // known once the strip device exists
    else
    {
        for (int i = 0; i < MAX_FANS; i++)
            if (g_cfg.pin[i] == g)
                r = "a fan header's PWM output";
    }
#if CONFIG_IDF_TARGET_ESP32C3
    if (!r && g >= 11 && g <= 17)
        r = "an SPI flash pad";
    else if (!r && (g == 18 || g == 19))
        r = "the USB pair (the host link)";
    else if (!r && (g == 8 || g == 9))
        r = "a boot strap pin";
#elif CONFIG_IDF_TARGET_ESP32
    if (!r && g >= 6 && g <= 11)
        r = "an SPI flash pad";
    else if (!r && (g == 1 || g == 3))
        r = "UART0 (the host link)";
    else if (!r && (g == 0 || g == 2 || g == 5 || g == 12 || g == 15))
        r = "a boot strap pin";
#endif
    if (why)
        *why = r;
    return r == nullptr;
}

// (re)configure the input pins after the standalone settings moved: every
// wired gpio header with a usable pin gets it as an input with the internal
// pull-up (a fan header's PWM is open-drain; unplugged, the pin then reads
// high = 100 % = the fan spec's own answer to a missing signal). A pin no
// longer used stays an input, which is harmless.
static void setupInputs()
{
    for (int i = 0; i < MAX_FANS; i++)
    {
        int8_t was = g_inPin[i];
        g_inPin[i] = -1;
        if (!g_wired[i] || g_sa.kind[i] != proto::FAN_KIND_GPIO)
            continue;

        const char* why = nullptr;
        if (!inputPinFree(g_sa.gpio[i], &why))
        {
            // once per change of settings, which is the only time this runs
            FLOG("header%d: source gpio:%u refused — %s; running the fallback",
                 i + 1, g_sa.gpio[i], why);
            continue;
        }

        g_inPin[i] = (int8_t)g_sa.gpio[i];
        if (was == g_inPin[i])
            continue;

        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << g_inPin[i];
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&io) != ESP_OK)
        {
            FLOG("header%d: gpio%d input init FAILED — running the fallback",
                 i + 1, g_inPin[i]);
            g_inPin[i] = -1;
            continue;
        }
        FLOG("header%d reads its PWM input on gpio%d (%u-point curve)", i + 1,
             g_inPin[i], g_sa.npts[i]);
        g_curveHave[i] = false;
    }

    // a header that stopped reading a pin stops running its curve now. With
    // no input pin left anywhere inputCheck() never reaches runCurves(), so
    // a stale g_curve would outrank the fallback for good — the header keeps
    // its last curve duty and the fallback slider does nothing
    for (int i = 0; i < MAX_FANS; i++)
        if (g_inPin[i] < 0)
        {
            g_curve[i] = NONE;
            g_curveHave[i] = false;
        }
}

// one reading of every input pin: a busy window of IN_WINDOW_US reading the
// GPIO input register, counting the samples each pin was high. The loop is a
// register read and a few bit tests, so a 2 ms window is tens of thousands
// of samples — well under a percent of jitter — and a task that preempts it
// mid-window only pauses the count at a random phase, which biases nothing.
// No interrupts and no peripheral: the PWM's frequency is irrelevant, and a
// constant level reads as 0 or 100 like any other duty.
static void sampleInputs()
{
    bool any = false;
    for (int i = 0; i < MAX_FANS; i++)
        any |= g_inPin[i] >= 0;
    if (!any)
    {
        memset(g_in, NONE, sizeof g_in);
        return;
    }

    uint32_t hi[MAX_FANS] = {};
    uint32_t total = 0;
    int64_t end = esp_timer_get_time() + IN_WINDOW_US;
    do
    {
        for (int k = 0; k < 16; k++)
        {
            uint32_t r0 = REG_READ(GPIO_IN_REG);
#if SOC_GPIO_PIN_COUNT > 32
            uint32_t r1 = REG_READ(GPIO_IN1_REG);
#endif
            for (int i = 0; i < MAX_FANS; i++)
            {
                int p = g_inPin[i];
                if (p < 0)
                    continue;
#if SOC_GPIO_PIN_COUNT > 32
                hi[i] += p >= 32 ? (r1 >> (p - 32)) & 1 : (r0 >> p) & 1;
#else
                hi[i] += (r0 >> p) & 1;
#endif
            }
            total++;
        }
    } while (esp_timer_get_time() < end);

    for (int i = 0; i < MAX_FANS; i++)
        g_in[i] = g_inPin[i] < 0 ? NONE : (uint8_t)((hi[i] * 100 + total / 2) / total);
}

// the board's own curves: each gpio header's reading through its curve, then
// the ramp, into g_curve. A header with no reading or no curve is NONE and
// falls through to the live duty or the fallback.
static void runCurves(float dt)
{
    for (int i = 0; i < MAX_FANS; i++)
    {
        if (!g_wired[i] || g_inPin[i] < 0 || g_in[i] == NONE || g_sa.npts[i] == 0)
        {
            g_curve[i] = NONE;
            g_curveHave[i] = false;
            continue;
        }

        fancurve::Point pts[POINTS];
        for (int j = 0; j < g_sa.npts[i]; j++)
            pts[j] = {(float)g_sa.pts[i][j][0], (float)g_sa.pts[i][j][1]};

        float target = fancurve::eval(pts, g_sa.npts[i], (float)g_in[i]);
        g_curveF[i] = fancurve::ramp(target, g_curveF[i], g_curveHave[i], (float)g_sa.ramp, dt);
        g_curveHave[i] = true;
        g_curve[i] = (uint8_t)(g_curveF[i] + 0.5f);
    }
}

// ---- NVS ----

// The host-pushed standalone settings persist so a daemon-less boot still
// runs the configured fallback and boost — layered over the fancfg defaults,
// which win again when re-flashed with different values (cfgstore.hpp)
static void loadSaved()
{
    uint8_t saved[Standalone::WIRE_LEN], base[Standalone::WIRE_LEN];
    g_cfg.sa.encode(base);
    if (!cfgstore::load(g_nvs, "sa", "sabase", base, sizeof base, saved))
        return;

    Standalone s = g_cfg.sa;
    s.merge(saved, sizeof saved);
    g_sa = s;
}

static void persist()
{
    uint8_t sa[Standalone::WIRE_LEN], base[Standalone::WIRE_LEN];
    g_sa.encode(sa);
    g_cfg.sa.encode(base);
    cfgstore::save(g_nvs, "sa", "sabase", sa, base, sizeof sa);
    g_saSeq = g_saSeq + 1;
}

// ---- the task ----

// drain whatever the host or the phone pushed since the last tick. Standalone
// settings persist (writes only on change, so a daemon restarting with the
// same config costs no flash wear); live duties never do
static void drainPending(uint32_t now)
{
    uint8_t sa[Standalone::WIRE_LEN], live[MAX_FANS], fb[MAX_FANS];
    PendSource src[MAX_FANS];
    uint16_t saLen;
    bool liveSet, shutdown, fbSet = false, srcSet = false;

    taskENTER_CRITICAL(&g_mux);
    saLen = g_pendSaLen;
    liveSet = g_pendLiveSet;
    shutdown = g_pendShutdown;
    if (saLen)
        memcpy(sa, g_pendSa, saLen);
    if (liveSet)
        memcpy(live, g_pendLive, sizeof live);
    memcpy(fb, g_pendFb, sizeof fb);
    memset(g_pendFb, NONE, sizeof g_pendFb);
    memcpy(src, g_pendSrc, sizeof src);
    memset(g_pendSrc, 0, sizeof g_pendSrc);
    g_pendSaLen = 0;
    g_pendLiveSet = g_pendShutdown = false;
    taskEXIT_CRITICAL(&g_mux);

    bool changed = false, saChanged = false;
    char b1[40], b2[40];

    if (saLen)
    {
        Standalone s = g_sa;
        s.merge(sa, saLen);
        if (!(s == g_sa))
        {
            g_sa = s;
            saChanged = true;
            FLOG("host set fallback %s, boost %s for %us, ramp %u%%/s",
                 fmtDuties(g_sa.duty, b1, sizeof b1),
                 fmtDuties(g_sa.boost, b2, sizeof b2), g_sa.boostSecs, g_sa.ramp);
        }
    }

    // the phone's edits land after the host's blob so that, in the one tick
    // both could arrive, the hand on the dial wins
    for (int i = 0; i < MAX_FANS; i++)
    {
        fbSet |= fb[i] != NONE;
        srcSet |= src[i].set;
    }
    if (fbSet || srcSet)
    {
        Standalone s = g_sa;
        for (int i = 0; i < MAX_FANS; i++)
        {
            if (fb[i] != NONE)
                s.duty[i] = fb[i];
            if (src[i].set)
                s.setSource(i, src[i].kind, src[i].gpio, src[i].npts, src[i].pts);
        }
        if (!(s == g_sa))
        {
            g_sa = s;
            saChanged = true;
            if (fbSet)
                FLOG("phone set fallback %s", fmtDuties(g_sa.duty, b1, sizeof b1));
            for (int i = 0; i < MAX_FANS; i++)
                if (src[i].set)
                    FLOG("phone set header%d source %s%u (%u points)", i + 1,
                         g_sa.kind[i] == proto::FAN_KIND_GPIO ? "gpio:" : "fallback ",
                         g_sa.kind[i] == proto::FAN_KIND_GPIO ? g_sa.gpio[i] : 0u,
                         g_sa.npts[i]);
        }
    }

    if (saChanged)
    {
        persist();
        setupInputs();
        changed = true;
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

// the gpio sources' turn: a reading every IN_PERIOD_MS, the curves and the
// ramp every tick (the ramp eases at percent per second, and 100 ms steps
// keep a slow-down smooth)
static void inputCheck(uint32_t now)
{
    bool any = false;
    for (int i = 0; i < MAX_FANS; i++)
        any |= g_inPin[i] >= 0;
    if (!any)
        return;

    if (!g_lastSample || now - g_lastSample >= IN_PERIOD_MS)
    {
        g_lastSample = now ? now : 1;
        sampleInputs();
    }

    runCurves(POLL_MS / 1000.0f);
    applyAll();
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
        inputCheck(now);
    }
}

// ---- public API ----

void setStandalone(const uint8_t* payload, uint16_t len)
{
    if (!g_started || len < Standalone::V1_LEN)
        return;

    // an older daemon sends the duties and boosts alone; the source part is
    // then left as it stands
    uint16_t n = len >= Standalone::WIRE_LEN ? Standalone::WIRE_LEN : Standalone::V1_LEN;

    taskENTER_CRITICAL(&g_mux);
    memcpy(g_pendSa, payload, n);
    g_pendSaLen = n;
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

bool setFallback(uint8_t slot, uint8_t pct)
{
    if (!g_started || slot >= MAX_FANS || !g_wired[slot] || pct > 100)
        return false;

    taskENTER_CRITICAL(&g_mux);
    g_pendFb[slot] = pct;
    taskEXIT_CRITICAL(&g_mux);
    return true;
}

bool setSource(uint8_t slot, uint8_t kind, uint8_t gpio, uint8_t npts,
               const uint8_t* pts, const char** why)
{
    const char* r = nullptr;
    if (!g_started || slot >= MAX_FANS || !g_wired[slot])
        r = "not a wired header";
    else if (kind != proto::FAN_KIND_FALLBACK && kind != proto::FAN_KIND_GPIO)
        r = "a source only the daemon can run";
    else if (kind == proto::FAN_KIND_GPIO && !Standalone::curveOk(npts, pts))
        r = "a malformed curve";
    else if (kind == proto::FAN_KIND_GPIO)
        inputPinFree(gpio, &r);

    if (why)
        *why = r;
    if (r)
        return false;

    PendSource s = {};
    s.set = true;
    s.kind = kind;
    s.gpio = kind == proto::FAN_KIND_GPIO ? gpio : NONE;
    s.npts = kind == proto::FAN_KIND_GPIO ? npts : 0;
    if (s.npts)
        memcpy(s.pts, pts, 2 * s.npts);

    taskENTER_CRITICAL(&g_mux);
    g_pendSrc[slot] = s;
    taskEXIT_CRITICAL(&g_mux);
    return true;
}

uint64_t inputPins()
{
    uint64_t m = 0;
    if (!g_started)
        return 0;
    for (int g = 0; g < GPIO_NUM_MAX && g < 64; g++)
        if (inputPinFree(g, nullptr))
            m |= 1ULL << g;
    return m;
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
            s.fallback[i] = NONE;
            s.kind[i] = NONE;
            s.in[i] = NONE;
            s.source[i] = SRC_NONE;
            continue;
        }
        s.duty[i] = effective(i);
        s.fallback[i] = g_sa.duty[i];
        s.kind[i] = g_sa.kind[i];
        s.in[i] = g_inPin[i] < 0 ? NONE : g_in[i];
        if (g_boosting && g_sa.boost[i] != NONE)
            s.source[i] = SRC_BOOST;
        else if (g_curve[i] != NONE)
            s.source[i] = SRC_CURVE;
        else if (g_live[i] != NONE)
        {
            s.source[i] = SRC_LIVE;
            s.live = true;
        }
        else
            s.source[i] = SRC_FALLBACK;
    }
}

uint16_t standalone(uint8_t* out, uint16_t max, uint32_t* seq)
{
    if (seq)
        *seq = g_saSeq;
    if (!g_started || max < Standalone::WIRE_LEN)
        return 0;
    g_sa.encode(out); // best-effort, as snapshot()
    return Standalone::WIRE_LEN;
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
    memset(g_pendFb, NONE, sizeof g_pendFb);
    memset(g_pendSrc, 0, sizeof g_pendSrc);
    memset(g_in, NONE, sizeof g_in);
    memset(g_curve, NONE, sizeof g_curve);
    memset(g_applied, NONE, sizeof g_applied);
    for (int i = 0; i < MAX_FANS; i++)
        g_inPin[i] = -1;
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
        else
            g_applied[i] = g_sa.duty[i];
    }

    char b1[40], b2[40], b3[40];
    uint8_t pins[MAX_FANS];
    for (int i = 0; i < MAX_FANS; i++)
        pins[i] = g_cfg.pin[i] < 0 ? NONE : (uint8_t)g_cfg.pin[i];
    FLOG("cfg %d headers, pins %s, fallback %s, boost %s for %us, ramp %u%%/s", count,
         fmtDuties(pins, b1, sizeof b1, false), fmtDuties(g_sa.duty, b2, sizeof b2),
         fmtDuties(g_sa.boost, b3, sizeof b3), g_sa.boostSecs, g_sa.ramp);

    // the gpio sources' input pins. render::pin() isn't known yet at this
    // point of boot (the strip comes up after us), so a configured input on
    // the strip's pin is the flasher's to catch; a runtime one is checked here
    setupInputs();

    g_started = true;

    // priority 2 like the power switch: a 100 ms duty tick never needs to win
    // against the LED service's latch cadence
    xTaskCreate(taskMain, "fan", 4096, nullptr, 2, nullptr);
}
} // namespace fan
