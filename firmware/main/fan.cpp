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
#include "fanwire.hpp"
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

// the gpio inputs' sampler (see sampleInputs): how often a reading is taken,
// and how long each one looks at the pins. A PC fan PWM is 21..28 kHz (a
// 36..48 µs period), so a 2 ms window averages some fifty periods; the input
// is read as a register, so one window covers every input pin at once
static const uint32_t IN_PERIOD_MS = 250;
static const int64_t IN_WINDOW_US = 2000;

// ---- the standalone settings (fancfg, NVS, CMD_FAN_STANDALONE) ----

// One shape on every side: which output each slot drives, what it runs when
// the daemon isn't driving it, and which slots this board runs a curve for
// itself — one fanwire::Header record per slot (protocol.hpp
// CMD_FAN_STANDALONE; the wire form, the NVS blob and the tail of the fancfg
// blob are all six of them). Kept here as arrays, so the task's loops read
// one field across the slots and the log can print a row of them.
struct Standalone
{
    uint8_t duty[MAX_FANS];      // resting duty percent per slot
    uint8_t boost[MAX_FANS];     // boost duty percent, NONE = sits the boost out
    uint8_t boostSecs[MAX_FANS]; // how long that boost runs after the host powers on
    uint8_t ramp[MAX_FANS];      // slow-down rate, whole percent per second (0 = instant)
    uint8_t kind[MAX_FANS];      // proto::FAN_KIND_*
    uint8_t gpio[MAX_FANS];      // a gpio slot's input pin, else NONE
    uint8_t npts[MAX_FANS];      // a gpio slot's curve: points (0 = none)
    uint8_t pts[MAX_FANS][POINTS][2]; // (input %, duty %), sorted by input
    uint8_t outKind[MAX_FANS];   // FAN_OUT_HEADER / FAN_OUT_GPIO, NONE = slot unused
    uint8_t out[MAX_FANS];       // the header number (1-based) or the GPIO

    static const uint16_t WIRE_LEN = proto::FAN_STANDALONE_LEN;

    Standalone()
    {
        for (int i = 0; i < MAX_FANS; i++)
            set(i, fanwire::Header::unused());
    }

    bool used(int i) const
    {
        return outKind[i] == proto::FAN_OUT_HEADER || outKind[i] == proto::FAN_OUT_GPIO;
    }

    fanwire::Header record(int i) const
    {
        fanwire::Header h;
        h.fallback = duty[i];
        h.boost = boost[i];
        h.boostSecs = boostSecs[i];
        h.ramp = ramp[i];
        h.kind = kind[i];
        h.gpio = gpio[i];
        h.npts = npts[i];
        memcpy(h.pts, pts[i], sizeof h.pts);
        h.outKind = outKind[i];
        h.out = out[i];
        return h;
    }

    void encode(uint8_t* p) const
    {
        for (int i = 0; i < MAX_FANS; i++)
            record(i).encode(p + i * fanwire::LEN);
    }

    // one slot's record in, clamped and checked: a duty past 100 is 100, a
    // kind past HOST is HOST, and a bad curve leaves the slot on its fallback
    // (a gpio slot with no curve runs that). A record that drives no output
    // is an unused slot: every field back to its 0xFF, full duty as the rest
    // (a slot with no pin drives nothing, so it is never applied)
    void set(int i, const fanwire::Header& h)
    {
        auto pct = [](uint8_t v) -> uint8_t { return v > 100 ? 100 : v; };
        if (!h.used())
        {
            duty[i] = 100;
            boost[i] = NONE;
            boostSecs[i] = proto::FAN_DEFAULT_BOOST_SECS;
            ramp[i] = proto::FAN_DEFAULT_RAMP;
            kind[i] = proto::FAN_KIND_HOST;
            gpio[i] = NONE;
            npts[i] = 0;
            memset(pts[i], 0, sizeof pts[i]);
            outKind[i] = NONE;
            out[i] = NONE;
            return;
        }
        duty[i] = pct(h.fallback);
        boost[i] = h.boost == NONE ? NONE : pct(h.boost);
        boostSecs[i] = h.boostSecs;
        ramp[i] = h.ramp;
        kind[i] = h.kind > proto::FAN_KIND_HOST ? proto::FAN_KIND_HOST : h.kind;
        gpio[i] = kind[i] == proto::FAN_KIND_GPIO ? h.gpio : NONE;
        npts[i] = 0;
        memset(pts[i], 0, sizeof pts[i]);
        if (kind[i] == proto::FAN_KIND_GPIO && h.curveOk())
        {
            npts[i] = h.npts;
            memcpy(pts[i], h.pts, 2 * h.npts);
        }
        outKind[i] = h.outKind;
        out[i] = h.out;
    }

    // a whole wire blob in: the truth for every slot (protocol.hpp)
    void load(const uint8_t* p)
    {
        for (int i = 0; i < MAX_FANS; i++)
            set(i, fanwire::Header::decode(p + i * fanwire::LEN));
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

// Same scheme as the power switch's `pwrcfg` (see power_switch.cpp): written
// at flash time by `make flash` / `make flash-fan` from the daemon config's
// "fans" block (tools/fancfg.py encodes it, and must match decode() below):
//
//     "FAN5" magic, then enabled(1) headerPin[6], then the Standalone wire
//     form (CMD_FAN_STANDALONE's six records, protocol.hpp)
//
// headerPin[n-1] is the GPIO of the board's header n (0xFF = the board has no
// such header) — the carrier's map (tools/pincheck.py FAN_PINS), a fact
// about the board, so it stays flash-time; which header or pin each slot
// drives is in the records and moves at runtime. enabled = the config has a
// fans block (even an empty list: the phone can add fans to a board with
// none). An erased partition (no magic) leaves the feature off. Earlier
// layouts (FAN1..FAN4) are not read: `make flash` writes the firmware and
// this partition together, so the two agree.

static const uint16_t BODY_LEN = 1 + MAX_FANS + Standalone::WIRE_LEN;

struct Config
{
    bool enabled = false;
    int8_t headerPin[MAX_FANS] = {-1, -1, -1, -1, -1, -1};
    Standalone sa;

    bool decode(const uint8_t* b, uint16_t len)
    {
        if (memcmp(b, "FAN5", 4) != 0 || len < 4 + BODY_LEN)
            return false;

        const uint8_t* p = b + 4;
        enabled = p[0] != 0;
        for (int i = 0; i < MAX_FANS; i++)
            headerPin[i] = (p[1 + i] == 0xFF || p[1 + i] >= GPIO_NUM_MAX) ? -1 : (int8_t)p[1 + i];
        sa.load(p + 1 + MAX_FANS);
        return true;
    }
};

static Config g_cfg; // loaded once in readConfig(), read-only after
static bool g_cfgLoaded = false, g_cfgRead = false, g_partitionFound = false;

// loadConfig returns false only when the partition itself is missing — the
// one case worth a log line, since it means the chip's partition table
// predates the feature and a `make flash-fan` would land in dead space.
// An erased or disabled config is the normal opted-out state and stays quiet.
static bool loadConfig(Config& c, bool& partitionFound)
{
    uint8_t b[4 + BODY_LEN];
    if (!cfgstore::partition("fancfg", b, sizeof b, &partitionFound))
        return false;

    return c.decode(b, sizeof b);
}

// ---- state ----

static nvs_handle_t g_nvs = 0;
static Standalone g_sa;         // standalone settings in force (flash
                                // defaults, overridden by a persisted push)
static volatile uint32_t g_saSeq = 1; // bumps when g_sa changes (standalone())
static uint8_t g_live[MAX_FANS]; // the daemon's live duties, NONE = not driven
static uint32_t g_liveMs = 0;    // millis() of the last live push
static bool g_hold = false;      // host announced shutdown: live never expires
static bool g_started = false;   // gate for the setters; set before the led_rx
                                 // task exists, so never raced

// the outputs: the pin each slot drives (-1 = none: unused, or a pin this
// board refused), and the same as a mask for the pin checks other tasks make
// (inputPinFree, outputPins). Written by the fan task (setupOutputs) — and
// before any task exists, by readConfig's plan — under g_pinMux; the mask is
// 64 bits, not one aligned word, so readers take the lock too.
static int8_t g_outPin[MAX_FANS] = {-1, -1, -1, -1, -1, -1};
static uint64_t g_outMask = 0;
static uint64_t g_inMask = 0;    // the gpio inputs' pins, likewise
static portMUX_TYPE g_pinMux = portMUX_INITIALIZER_UNLOCKED;

// the gpio inputs: the pin each slot samples (-1 = none: not a gpio slot, or
// a pin this board can't read), the last reading, and the curve's ramped
// output
static int8_t g_inPin[MAX_FANS] = {-1, -1, -1, -1, -1, -1};
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
static bool g_pendSaSet = false;
static uint8_t g_pendLive[MAX_FANS];
static bool g_pendLiveSet = false;
static bool g_pendShutdown = false;
struct PendHeader // the phone's edit of one slot: its whole record
{
    bool set;
    fanwire::Header h;
};
static PendHeader g_pendHdr[MAX_FANS];

// boost state (see boostCheck): with the power switch present, an armed
// one-shot keyed to its power-on events; without it, an edge detector on USB
// SOF presence (which the UART build hardcodes true, so the boost fires once
// at task start there). One clock starts the windows; each slot's own boost
// length ends its window
static bool g_boostOn[MAX_FANS];   // slot i is in its boost window
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

// the duty slot i runs right now: boost, then this board's own curve, then
// the daemon's live duty, then resting
static uint8_t effective(int i)
{
    if (g_boostOn[i])
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
        if (g_outPin[i] < 0)
            continue;
        uint8_t d = effective(i);
        if (d == g_applied[i])
            continue;
        g_applied[i] = d;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, dutyOf(d));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
}

// "65/-/40/-/-/-" style for the log — a dash for a slot that drives nothing
// (or, for live/boost, isn't set)
static const char* fmtDuties(const uint8_t* d, char* buf, size_t n,
                             bool onlyUsed = true)
{
    size_t at = 0;
    for (int i = 0; i < MAX_FANS && at + 5 < n; i++)
    {
        if ((onlyUsed && g_outPin[i] < 0) || d[i] == NONE)
            at += snprintf(buf + at, n - at, "%s-", i ? "/" : "");
        else
            at += snprintf(buf + at, n - at, "%s%u", i ? "/" : "", d[i]);
    }
    return buf;
}

// ---- the pins ----

static uint64_t bit(int g) { return g >= 0 && g < 64 ? 1ULL << g : 0; }

static uint64_t outMask()
{
    taskENTER_CRITICAL(&g_pinMux);
    uint64_t m = g_outMask;
    taskEXIT_CRITICAL(&g_pinMux);
    return m;
}

static uint64_t inMask()
{
    taskENTER_CRITICAL(&g_pinMux);
    uint64_t m = g_inMask;
    taskEXIT_CRITICAL(&g_pinMux);
    return m;
}

// is g one of the board's header pins? (a fan connector's PWM wire, whether
// or not a fan drives it right now)
static bool headerPin(int g)
{
    for (int i = 0; i < MAX_FANS; i++)
        if (g_cfg.headerPin[i] >= 0 && g_cfg.headerPin[i] == g)
            return true;
    return false;
}

// what hurts any pin on this chip: the flash pads, the host link, the straps
// (straps only where `straps` — a board's own header pins are its maker's
// call, checked when flashing)
static const char* chipPinProblem(int g, bool straps)
{
    if (g < 0 || g >= GPIO_NUM_MAX || !GPIO_IS_VALID_GPIO(g))
        return "not a GPIO on this chip";
#if CONFIG_IDF_TARGET_ESP32C3
    if (g >= 11 && g <= 17)
        return "an SPI flash pad";
    if (g == 18 || g == 19)
        return "the USB pair (the host link)";
    if (straps && (g == 8 || g == 9))
        return "a boot strap pin";
#elif CONFIG_IDF_TARGET_ESP32
    if (g >= 6 && g <= 11)
        return "an SPI flash pad";
    if (g == 1 || g == 3)
        return "UART0 (the host link)";
    if (straps && (g == 0 || g == 2 || g == 5 || g == 12 || g == 15))
        return "a boot strap pin";
#else
    (void)straps;
#endif
    return nullptr;
}

// the strip's data pin: the live device's once it exists, before that the one
// the LED service will bring it up on at boot (its saved geometry, read in
// readConfig — the fans start ahead of the strip, and a slot saved onto the
// strip's pin must not be attached then). -1 = not known.
static int g_bootStripPin = -1;
static int stripPin() { return render::up() ? render::pin() : g_bootStripPin; }

// can this board read an input on GPIO g (fan.hpp)? The flasher checks a
// configured pin against the same facts (tools/fancfg.py, pincheck.py); this
// is the check for a pin that arrives at runtime — a daemon push, a phone
// edit, the power switch's wake pin — and the one place the phone's edit can
// be refused with a reason. Pins that would hurt: another feature's, a fan
// header's (a fan connector's wire, driven or not), one of our outputs, the
// flash pads, the host link, and the boot straps (a PWM at 0 % is a pin held
// low at reset).
// ...against the outputs `outs` (the ones in force, or the ones a phone's
// edit would leave)
static bool inputPinFreeWith(int g, uint64_t outs, const char** why)
{
    const char* r = chipPinProblem(g, true);
    if (!r && pwr::usesPin(g))
        r = "the power switch's pin";
    else if (!r && stripPin() == g)
        r = "the strip's data pin"; // known once the strip device exists
    else if (!r && headerPin(g))
        r = "a fan header's PWM wire";
    else if (!r && (outs & bit(g)))
        r = "a fan's PWM output";
    if (why)
        *why = r;
    return r == nullptr;
}

bool inputPinFree(int g, const char** why) { return inputPinFreeWith(g, outMask(), why); }

// can slot `slot` drive a PWM out on GPIO g? (-1: any new slot.) Everything
// that hurts an input, plus: an input-only pad, a pin some slot READS, and a
// pin another slot drives already. A header pin passes the strap check — the
// board wires it, and the flasher checked it.
static bool outputPinFree(int g, int slot, const uint64_t outs, const uint64_t ins, const char** why)
{
    const char* r = chipPinProblem(g, !headerPin(g));
    if (!r && !GPIO_IS_VALID_OUTPUT_GPIO(g))
        r = "an input-only pad";
    else if (!r && pwr::usesPin(g))
        r = "the power switch's pin";
    else if (!r && stripPin() == g)
        r = "the strip's data pin";
    else if (!r && (ins & bit(g)))
        r = "a fan's PWM input";
    else if (!r && (outs & bit(g)) && !(slot >= 0 && g_outPin[slot] == g))
        r = "another fan's output";
    if (why)
        *why = r;
    return r == nullptr;
}

// the pin a record's output names (-1 with the reason when it names none this
// board has)
static int pinOf(uint8_t outKind, uint8_t out, const char** why)
{
    if (outKind == proto::FAN_OUT_HEADER)
    {
        if (out < 1 || out > MAX_FANS || g_cfg.headerPin[out - 1] < 0)
        {
            *why = "no such header on this board";
            return -1;
        }
        return g_cfg.headerPin[out - 1];
    }
    if (outKind == proto::FAN_OUT_GPIO)
        return out;
    *why = nullptr;
    return -1;
}

// the input pins the settings name (gpio slots), as a mask
static uint64_t wantedInputs(const Standalone& s)
{
    uint64_t m = 0;
    for (int i = 0; i < MAX_FANS; i++)
        if (s.used(i) && s.kind[i] == proto::FAN_KIND_GPIO)
            m |= bit(s.gpio[i]);
    return m;
}

// the pins slots should drive under settings `s`, each checked against the
// others in slot order (the earlier slot keeps a contested pin) and against
// everything outputPinFree knows. `log` says each refusal (only on a change of
// settings, the only time this runs)
static void planOutputs(const Standalone& s, int8_t* pins, bool log)
{
    uint64_t taken = 0, ins = wantedInputs(s);
    for (int i = 0; i < MAX_FANS; i++)
    {
        pins[i] = -1;
        if (!s.used(i))
            continue;
        const char* why = nullptr;
        int g = pinOf(s.outKind[i], s.out[i], &why);
        if (g >= 0 && (taken & bit(g)))
            why = "another fan's output";
        else if (g >= 0)
            outputPinFree(g, -1, 0, ins, &why);
        if (why || g < 0)
        {
            if (log)
                FLOG("slot %d: output %s%u refused — %s; not driven", i,
                     s.outKind[i] == proto::FAN_OUT_HEADER ? "header" : "gpio:", s.out[i],
                     why ? why : "no pin");
            continue;
        }
        pins[i] = (int8_t)g;
        taken |= bit(g);
    }
}

// (re)attach the outputs after the standalone settings moved: every slot's
// pin as planned — a pin no slot drives any more is released first (its
// channel stopped, the pin reset: undriven, so a 4-pin fan on it runs full,
// the fan spec's answer to a floating PWM wire), then the new ones attached
// at the duty they run. A slot that moves to another fan keeps nothing of
// the old one's curve or ramp
static void setupOutputs()
{
    int8_t want[MAX_FANS];
    planOutputs(g_sa, want, true);

    for (int i = 0; i < MAX_FANS; i++)
        if (g_outPin[i] >= 0 && want[i] != g_outPin[i])
        {
            ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, 1);
            gpio_reset_pin((gpio_num_t)g_outPin[i]);
            FLOG("slot %d: released gpio%d", i, g_outPin[i]);
            taskENTER_CRITICAL(&g_pinMux);
            g_outMask &= ~bit(g_outPin[i]);
            g_outPin[i] = -1;
            taskEXIT_CRITICAL(&g_pinMux);
            g_applied[i] = NONE;
            g_curve[i] = NONE;
            g_curveHave[i] = false;
            g_boostOn[i] = false;
            // the live duty was the old occupant's (slots shift when a fan
            // before them is deleted): the new one runs its fallback until
            // the daemon's next live push names its own
            g_live[i] = NONE;
        }

    for (int i = 0; i < MAX_FANS; i++)
    {
        if (want[i] < 0 || want[i] == g_outPin[i])
            continue;
        // a fan attached inside the power-on boost window joins it for the
        // time left (a slot re-pinned by an edit then keeps its boost)
        g_boostOn[i] = g_boostStart && g_sa.boost[i] != NONE && g_sa.boostSecs[i] > 0 &&
                       millis() - g_boostStart < (uint32_t)g_sa.boostSecs[i] * 1000;
        ledc_channel_config_t cc = {};
        cc.gpio_num = want[i];
        cc.speed_mode = LEDC_LOW_SPEED_MODE;
        cc.channel = (ledc_channel_t)i;
        cc.timer_sel = LEDC_TIMER_0;
        cc.duty = dutyOf(effective(i));
        cc.hpoint = 0;
        if (ledc_channel_config(&cc) != ESP_OK)
        {
            FLOG("slot %d: gpio%d init FAILED — not driven", i, want[i]);
            continue;
        }
        g_applied[i] = effective(i);
        taskENTER_CRITICAL(&g_pinMux);
        g_outPin[i] = want[i];
        g_outMask |= bit(want[i]);
        taskEXIT_CRITICAL(&g_pinMux);
        FLOG("slot %d drives gpio%d (%s%u) at %u%%", i, want[i],
             g_sa.outKind[i] == proto::FAN_OUT_HEADER ? "header" : "gpio:", g_sa.out[i], g_applied[i]);
    }
}

// (re)configure the input pins after the standalone settings moved: every
// gpio slot with a usable pin gets it as an input with the internal pull-up
// (a fan header's PWM is open-drain; unplugged, the pin then reads high =
// 100 % = the fan spec's own answer to a missing signal). A pin no longer used
// stays an input, which is harmless. Runs after setupOutputs, so a pin that
// just became an output is refused here.
static void setupInputs()
{
    uint64_t mask = 0;
    for (int i = 0; i < MAX_FANS; i++)
    {
        int8_t was = g_inPin[i];
        g_inPin[i] = -1;
        if (g_outPin[i] < 0 || g_sa.kind[i] != proto::FAN_KIND_GPIO)
            continue;

        const char* why = nullptr;
        if (!inputPinFree(g_sa.gpio[i], &why))
        {
            // once per change of settings, which is the only time this runs
            FLOG("slot %d: input gpio:%u refused — %s; running the fallback",
                 i, g_sa.gpio[i], why);
            continue;
        }

        g_inPin[i] = (int8_t)g_sa.gpio[i];
        mask |= bit(g_inPin[i]);
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
            FLOG("slot %d: gpio%d input init FAILED — running the fallback",
                 i, g_inPin[i]);
            g_inPin[i] = -1;
            continue;
        }
        FLOG("slot %d reads its PWM input on gpio%d (%u-point curve)", i,
             g_inPin[i], g_sa.npts[i]);
        g_curveHave[i] = false;
    }
    taskENTER_CRITICAL(&g_pinMux);
    g_inMask = mask;
    taskEXIT_CRITICAL(&g_pinMux);

    // a slot that stopped reading a pin stops running its curve now. With no
    // input pin left anywhere inputCheck() never reaches runCurves(), so a
    // stale g_curve would outrank the fallback for good — the slot keeps its
    // last curve duty and the fallback slider does nothing
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

// the board's own curves: each gpio slot's reading through its curve, then
// the ramp, into g_curve. A slot with no reading or no curve is NONE and
// falls through to the live duty or the fallback.
static void runCurves(float dt)
{
    for (int i = 0; i < MAX_FANS; i++)
    {
        if (g_outPin[i] < 0 || g_inPin[i] < 0 || g_in[i] == NONE || g_sa.npts[i] == 0)
        {
            g_curve[i] = NONE;
            g_curveHave[i] = false;
            continue;
        }

        fancurve::Point pts[POINTS];
        for (int j = 0; j < g_sa.npts[i]; j++)
            pts[j] = {(float)g_sa.pts[i][j][0], (float)g_sa.pts[i][j][1]};

        float target = fancurve::eval(pts, g_sa.npts[i], (float)g_in[i]);
        g_curveF[i] = fancurve::ramp(target, g_curveF[i], g_curveHave[i], (float)g_sa.ramp[i], dt);
        g_curveHave[i] = true;
        g_curve[i] = (uint8_t)(g_curveF[i] + 0.5f);
    }
}

// ---- NVS ----

// The host-pushed standalone settings persist so a daemon-less boot still
// runs the configured fans — layered over the fancfg defaults, which win
// again when re-flashed with different values (cfgstore.hpp)
static void loadSaved()
{
    uint8_t saved[Standalone::WIRE_LEN], base[Standalone::WIRE_LEN];
    g_cfg.sa.encode(base);
    if (!cfgstore::load(g_nvs, "sa", "sabase", base, sizeof base, saved))
        return;

    g_sa.load(saved);
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
    uint8_t sa[Standalone::WIRE_LEN], live[MAX_FANS];
    PendHeader hdr[MAX_FANS];
    bool saSet, liveSet, shutdown;

    taskENTER_CRITICAL(&g_mux);
    saSet = g_pendSaSet;
    liveSet = g_pendLiveSet;
    shutdown = g_pendShutdown;
    if (saSet)
        memcpy(sa, g_pendSa, sizeof sa);
    if (liveSet)
        memcpy(live, g_pendLive, sizeof live);
    for (int i = 0; i < MAX_FANS; i++)
    {
        hdr[i] = g_pendHdr[i];
        g_pendHdr[i].set = false;
    }
    g_pendSaSet = g_pendLiveSet = g_pendShutdown = false;
    taskEXIT_CRITICAL(&g_mux);

    bool changed = false, saChanged = false;
    char b1[40], b2[40], b3[40], b4[40];

    if (saSet)
    {
        Standalone s;
        s.load(sa);
        if (!(s == g_sa))
        {
            g_sa = s;
            saChanged = true;
            uint8_t outs[MAX_FANS];
            for (int i = 0; i < MAX_FANS; i++)
                outs[i] = g_sa.used(i) ? g_sa.out[i] : NONE;
            FLOG("host set outputs %s, fallback %s, boost %s for %s s",
                 fmtDuties(outs, b1, sizeof b1, false), fmtDuties(g_sa.duty, b2, sizeof b2, false),
                 fmtDuties(g_sa.boost, b3, sizeof b3, false), fmtDuties(g_sa.boostSecs, b4, sizeof b4, false));
        }
    }

    // the phone's edits land after the host's blob so that, in the one tick
    // both could arrive, the hand on the dial wins
    for (int i = 0; i < MAX_FANS; i++)
    {
        if (!hdr[i].set)
            continue;
        Standalone s = g_sa;
        s.set(i, hdr[i].h);
        if (s == g_sa)
            continue;
        g_sa = s;
        saChanged = true;
        if (!g_sa.used(i))
            FLOG("phone removed slot %d's fan", i);
        else
        {
            if (g_sa.boost[i] != NONE)
                snprintf(b2, sizeof b2, "%u%%", (unsigned)g_sa.boost[i]);
            FLOG("phone set slot %d: output %s%u, fallback %u%%, input %s%u (%u points), ramp %u%%/s, boost %s for %us",
                 i, g_sa.outKind[i] == proto::FAN_OUT_HEADER ? "header" : "gpio:", g_sa.out[i], g_sa.duty[i],
                 g_sa.kind[i] == proto::FAN_KIND_GPIO ? "gpio:" : g_sa.kind[i] == proto::FAN_KIND_FALLBACK ? "fallback " : "host ",
                 g_sa.kind[i] == proto::FAN_KIND_GPIO ? g_sa.gpio[i] : 0u, g_sa.npts[i], g_sa.ramp[i],
                 g_sa.boost[i] == NONE ? "-" : b2, g_sa.boostSecs[i]);
        }
    }

    if (saChanged)
    {
        persist();
        setupOutputs();
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
// slots run their fallback until the new daemon pushes.
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
        memset(g_live, NONE, sizeof g_live);
        g_liveMs = 0;
        g_hold = false;

        bool any = false;
        for (int i = 0; i < MAX_FANS; i++)
        {
            g_boostOn[i] = g_outPin[i] >= 0 && g_sa.boost[i] != NONE && g_sa.boostSecs[i] > 0;
            any |= g_boostOn[i];
        }
        g_boostStart = now ? now : 1;

        char b1[40], b2[40];
        if (any)
            FLOG("boost: host powered on — %s for %s s (source=%s)",
                 fmtDuties(g_sa.boost, b1, sizeof b1), fmtDuties(g_sa.boostSecs, b2, sizeof b2),
                 s >= 0 ? "sense" : "usb");
        applyAll();
    }

    bool ended = false;
    for (int i = 0; i < MAX_FANS; i++)
    {
        if (!g_boostOn[i] || now - g_boostStart < (uint32_t)g_sa.boostSecs[i] * 1000)
            continue;
        g_boostOn[i] = false;
        ended = true;
        FLOG("slot %d boost done — settling to %u%%", i, effective(i));
    }
    if (ended)
        applyAll();
}

// the gpio inputs' turn: a reading every IN_PERIOD_MS, the curves and the
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
    if (!g_started || len != Standalone::WIRE_LEN)
        return;

    taskENTER_CRITICAL(&g_mux);
    memcpy(g_pendSa, payload, Standalone::WIRE_LEN);
    g_pendSaSet = true;
    // a live push still queued came BEFORE this: its duties are laid out for
    // the slots as they were, and applied after the new layout (drainPending
    // takes the standalone first) they would land on the wrong fans once a
    // delete shifted them. Dropped; the daemon resends its live duties on
    // the tick after every standalone (fans.hpp pushStandalone)
    g_pendLiveSet = false;
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

bool setHeader(uint8_t slot, const uint8_t* rec, uint16_t len, const char** why)
{
    const char* r = nullptr;
    fanwire::Header h;
    if (len == fanwire::LEN)
        h = fanwire::Header::decode(rec);

    if (!g_started)
        r = "the fan feature is off";
    else if (slot >= MAX_FANS)
        r = "not a slot";
    else if (len != fanwire::LEN)
        r = "not a slot record";
    else if (!h.used())
        ; // the fan removed: nothing more to check
    else if (h.fallback > 100)
        r = "a fallback past 100 %";
    else if (h.boost != NONE && h.boost > 100)
        r = "a boost past 100 %";
    else if (h.kind > proto::FAN_KIND_HOST)
        r = "not an input kind";
    else if (h.kind == proto::FAN_KIND_GPIO && !h.curveOk())
        r = "a malformed curve";
    else
    {
        // the output, against the other slots as they stand (this one's own
        // pin and input excluded — it is what the record replaces)
        const char* pw = nullptr;
        int g = pinOf(h.outKind, h.out, &pw);
        uint64_t outs = outMask() & ~bit(g_outPin[slot]);
        uint64_t ins = 0;
        for (int i = 0; i < MAX_FANS; i++)
            if (i != (int)slot && g_inPin[i] >= 0)
                ins |= bit(g_inPin[i]);
        if (g < 0)
            r = pw ? pw : "no output";
        else if (h.kind == proto::FAN_KIND_GPIO && h.gpio == g)
            r = "the same pin as the fan's output";
        else
            outputPinFree(g, slot, outs, ins, &r);
        // its input: one this board can read, and not a pin the outputs
        // (the others', and this record's own) would drive
        if (!r && h.kind == proto::FAN_KIND_GPIO)
            inputPinFreeWith(h.gpio, outs | bit(g), &r);
    }

    if (why)
        *why = r;
    if (r)
        return false;

    if (!h.used())
        h = fanwire::Header::unused();
    else if (h.kind != proto::FAN_KIND_GPIO)
    {
        h.gpio = NONE;
        h.npts = 0;
        memset(h.pts, 0, sizeof h.pts);
    }

    taskENTER_CRITICAL(&g_mux);
    g_pendHdr[slot].set = true;
    g_pendHdr[slot].h = h;
    taskEXIT_CRITICAL(&g_mux);
    return true;
}

bool readsPin(int g)
{
    return g >= 0 && (inMask() & bit(g));
}

uint64_t inputPins()
{
    uint64_t m = 0;
    for (int g = 0; g < GPIO_NUM_MAX && g < 64; g++)
        if (inputPinFree(g, nullptr))
            m |= 1ULL << g;
    return m;
}

uint64_t outputPins()
{
    uint64_t m = 0, outs = outMask(), ins = inMask();
    for (int g = 0; g < GPIO_NUM_MAX && g < 64; g++)
        if (!headerPin(g) && outputPinFree(g, -1, outs, ins, nullptr))
            m |= 1ULL << g;
    return m;
}

void headerPins(uint8_t* out)
{
    for (int i = 0; i < MAX_FANS; i++)
        out[i] = g_cfg.headerPin[i] < 0 ? NONE : (uint8_t)g_cfg.headerPin[i];
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
    // that straddles a tick can pair one slot's new duty with another's old
    // one for one dashboard poll, which is all the view is for
    s.active = g_started;
    s.boosting = false;
    s.hold = g_hold;
    s.live = false;
    for (int i = 0; i < MAX_FANS; i++)
    {
        s.wired[i] = g_started && g_outPin[i] >= 0;
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
        if (g_boostOn[i])
        {
            s.source[i] = SRC_BOOST;
            s.boosting = true;
        }
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

void readConfig()
{
    if (g_cfgRead)
        return;
    g_cfgRead = true;
    // the LED service's saved strip geometry (led_service.cpp: namespace
    // "ledrx", saved only once a strip has run, so no count = no pin yet)
    nvs_handle_t led;
    if (nvs_open("ledrx", NVS_READONLY, &led) == ESP_OK)
    {
        uint16_t count = 0;
        uint8_t pin = 0;
        if (nvs_get_u16(led, "count", &count) == ESP_OK && count > 0 && nvs_get_u8(led, "pin", &pin) == ESP_OK)
            g_bootStripPin = pin;
        nvs_close(led);
    }

    g_cfgLoaded = loadConfig(g_cfg, g_partitionFound);
    if (!g_cfgLoaded || !g_cfg.enabled)
        return;

    // the settings in force (the partition's, under a persisted push) and
    // the pins they will drive, planned now — before the power switch starts
    // and checks a saved wake pin against inputPinFree(), which must already
    // know where the outputs are. start() attaches them (and re-checks them
    // against the power switch's pins, which are known then)
    g_sa = g_cfg.sa;
    if (nvs_open("fan", NVS_READWRITE, &g_nvs) == ESP_OK)
        loadSaved();
    int8_t plan[MAX_FANS];
    planOutputs(g_sa, plan, false);
    uint64_t m = 0;
    for (int i = 0; i < MAX_FANS; i++)
        m |= bit(plan[i]);
    taskENTER_CRITICAL(&g_pinMux);
    g_outMask = m;
    g_inMask = wantedInputs(g_sa);
    taskEXIT_CRITICAL(&g_pinMux);
}

void start()
{
    readConfig();
    bool partitionFound = g_partitionFound, loaded = g_cfgLoaded;

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
    {
        // erased or switched off: the normal opted-out state, no pins held
        taskENTER_CRITICAL(&g_pinMux);
        g_outMask = g_inMask = 0;
        taskEXIT_CRITICAL(&g_pinMux);
        return;
    }

    memset(g_live, NONE, sizeof g_live);
    for (auto& h : g_pendHdr)
        h.set = false;
    memset(g_boostOn, 0, sizeof g_boostOn);
    memset(g_in, NONE, sizeof g_in);
    memset(g_curve, NONE, sizeof g_curve);
    memset(g_applied, NONE, sizeof g_applied);
    // the plan readConfig made is only a plan: nothing is attached yet
    taskENTER_CRITICAL(&g_pinMux);
    g_outMask = 0;
    taskEXIT_CRITICAL(&g_pinMux);

    // one timer at the fan frequency, one channel per slot (slot i = LEDC
    // channel i). All LEDC on the C3 is the one low-speed group; the strip's
    // SPI (or RMT) is a different peripheral entirely, so the two never
    // contend.
    ledc_timer_config_t tc = {};
    tc.speed_mode = LEDC_LOW_SPEED_MODE;
    tc.duty_resolution = PWM_RES;
    tc.timer_num = LEDC_TIMER_0;
    tc.freq_hz = PWM_HZ;
    tc.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&tc) != ESP_OK)
    {
        FLOG("LEDC timer init FAILED — feature off");
        // readConfig's planned inputs go too: nothing reads them now, and
        // held they'd keep the wake pin and the phone's free-pin lists off
        // pins no fan uses
        taskENTER_CRITICAL(&g_pinMux);
        g_inMask = 0;
        taskEXIT_CRITICAL(&g_pinMux);
        return;
    }

    // the outputs and the gpio inputs. The strip isn't up yet at this point
    // of boot, so its pin is the one it saved (stripPin)
    setupOutputs();
    setupInputs();

    char b1[40], b2[40], b3[40], b4[40], b5[40];
    uint8_t pins[MAX_FANS];
    for (int i = 0; i < MAX_FANS; i++)
        pins[i] = g_outPin[i] < 0 ? NONE : (uint8_t)g_outPin[i];
    FLOG("cfg outputs %s, fallback %s, boost %s for %s s, ramp %s %%/s",
         fmtDuties(pins, b1, sizeof b1, false), fmtDuties(g_sa.duty, b2, sizeof b2),
         fmtDuties(g_sa.boost, b3, sizeof b3), fmtDuties(g_sa.boostSecs, b4, sizeof b4),
         fmtDuties(g_sa.ramp, b5, sizeof b5));

    g_started = true;

    // priority 2 like the power switch: a 100 ms duty tick never needs to win
    // against the LED service's latch cadence
    xTaskCreate(taskMain, "fan", 4096, nullptr, 2, nullptr);
}
} // namespace fan
