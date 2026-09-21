#include "power_switch.hpp"

#include "fan.hpp"

#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32C3
#include "soc/rtc_cntl_reg.h"
#endif
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_system.h"
#include "nvs.h"

#include "protocol.hpp"

#include "cfgstore.hpp"
#include "dbglog.hpp"
#include "hostreq.hpp"
#include "util.hpp"

// Diagnostics go to the in-RAM debug log (dbglog.hpp), which the daemon drains
// into journalctl when its debug backchannel is on. Off by default and near-
// free: event lines are rare, and the chatty per-sample line is gated on
// dbglog::active() so an un-watched board never formats it.
#define PLOG(fmt, ...) dbglog::line("pwr: " fmt, ##__VA_ARGS__)

// The control model (after Thunkar/bc250-esp32-switch, minus its WiFi/BLE):
//
//   * PS_ON# is active LOW and pulled up to 5 V inside the PSU. The pin does
//     not touch it directly — a 3.3 V pad can't sit on a 5 V-pulled line — but
//     drives the gate of an N-channel MOSFET that sinks PS_ON# to ground:
//     gate HIGH = PSU on, gate LOW = PSU off (see applyPsOn). A gate pull-down
//     keeps the MOSFET off whenever the pad isn't driving.
//   * The button reads with an internal pull-up (pressed = LOW); an optional
//     second pin is driven LOW as the button's local ground, so a two-wire
//     switch needs no run to a real GND pin. It powers on the *rising* edge of
//     the press (as soon as the press debounces, not on release) so that
//     keeping it held afterwards is free to mean something else — see the
//     failsafe below.
//   * The optional WAKE input is a second way to power on and nothing else:
//     a 3.3 V active-HIGH pulse from another device that wants the machine up
//     — an OpenPuck (github.com/safijari/openpuck), whose ColdBoot drives its
//     GPIO high for 300 ms when the paired Steam Controller's Steam button is
//     pressed while the host reads USB-unmounted. On a motherboard that pulse
//     closes the power-switch header, which means "on" or "shut down"
//     depending on state; here it only ever means "on" — a pulse in BOOTING or
//     ON is logged and dropped, so nothing the puck does can shut the machine
//     down, whatever its own gating decides. Read with the internal pull-down
//     (an unpowered or unplugged puck reads idle), debounced like the button,
//     edge-triggered, and ARMED only once it has read idle for WAKE_ARM_MS:
//     both boards come up together on 5VSB and the puck's pin floats until its
//     firmware sets it, and a level held high across one of our resets is not
//     a fresh press either. The feedback LED blinks through the pulse, so the
//     wiring can be eyeballed without a PSU, as with the button.
//   * The sense wire (TPMS1 pin 9 on the BC-250) is the board's main 3.3 V
//     rail — a stiff, well-decoupled node, NOT a soft signal line. It is the
//     right thing to sense because the BC-250 runs on 12 V alone and derives
//     both its 3.3 V rails on board: while we hold PS_ON# the 12 V input and
//     TPMS1's 3VSB (pin 15) stay up, and only this rail collapses when the OS
//     powers itself down. Read as an averaged ADC voltage with hysteresis
//     rather than digitally — a rail would read fine digitally, but the mV
//     window also catches a half-collapsed rail and makes the level visible
//     in the debug log. Note the ADC saturates near 3.1 V at 12 dB
//     attenuation, so a healthy 3.3 V rail logs as ~2.9-3.1 V, not 3300.
//   * Do NOT sense TPMS1 pin 15 (3VSB): it stays up whenever PS_ON# is held,
//     so it reads exactly like a working sense wire and then silently never
//     fires the follow-down or the boot timeout.
//   * A SHORT press while the machine is up is the ordinary PC power-button
//     gesture: shut the OS down gracefully. Nothing here can do that — only the
//     OS can — so it asks, over the link, and the daemon runs its poweroff
//     command (hostreq.hpp, and "power_switch.short_press" on the host). We
//     then do nothing at all: the board powers itself off, the sense line drops,
//     and the follow-down below cuts the PSU. If nobody answers within
//     REQ_ACK_MS — no daemon, no OS, the host feature left off — the request is
//     dropped and the feedback LED says so, because otherwise an unheard press
//     is indistinguishable from a dead button.
//   * "Off" here means cutting the PSU — a hard power-off, not a graceful OS
//     shutdown; hence the hold-to-fire threshold, and why a press that ends
//     early is the graceful one. Holding stays the only hard cut, which is also
//     the answer to an OS that accepts the request and then wedges.
//   * FAILSAFE: while the button is physically held, the sense line cannot
//     release PS_ON# — no follow-down, and the boot timeout counts from the
//     release rather than from the power-on. A sense wire that reads low while
//     the board is really up (wrong pin, wire off, thresholds off) is
//     otherwise self-sealing: the boot timeout cuts the PSU ~10 s after every
//     power-on, so the machine can never stay up long enough to reflash the
//     config that would fix it. Press to power on and just keep holding — the
//     machine stays up for as long as your finger does, which is long enough
//     to reflash. Sense is still sampled and logged while held; that log is
//     how the right pin and thresholds get found.
//
// One deliberate difference from a plain switch: the intended PSU state is
// persisted, and any reset that isn't a true power-on (crash, watchdog, a
// reflash) re-asserts PS_ON# immediately in start() — the board's power hangs
// on this pin, so a firmware hiccup must not drop it. gpio_hold_en() latches
// the asserted level in the always-on domain as well, so on pins that support
// it the line even rides through the reset itself — and that latch doubles as
// a second record of the intent that survives anything written to flash (see
// psOnHeld). A true power-on reset
// (5VSB was lost) clears the intent instead: everything genuinely lost power,
// and the board should not boot just because standby came back.
namespace pwr
{
// ---- fixed tuning (values proven on the BC-250 by the reference project) ----

static const uint32_t POLL_MS = 10;             // task cadence; all debounces count in these
static const uint32_t DEBOUNCE_MS = 30;         // button, wake, and sense going UP
static const uint32_t WAKE_ARM_MS = 1000;       // wake input must read idle this long before
                                                // its first edge counts (floats during the
                                                // puck's own boot)
static const uint32_t LED_BLINK_MS = 100;       // feedback LED half-period while pressed
static const uint32_t BOARD_OFF_DEBOUNCE_MS = 1500; // sense must stay down this long
                                                    // (filters dips during boot/reset)
static const uint32_t REQ_ACK_MS = 3000;      // host must answer a shutdown request within this
static const uint32_t NAK_BLINK_MS = 1500;    // ...or the LED blinks this long to report it
static const uint32_t NAK_BLINK_HALF_MS = 60; // fast, so it can't be read as a press blink
static const int SENSE_OVERSAMPLE = 16; // ADC reads averaged per sample: TPMS1 is
                                        // high-impedance and one-shot reads spike, and a
                                        // single spike past the hysteresis would restart
                                        // the board-off debounce forever

// ---- configuration (the `pwrcfg` flash partition) ----

// The wiring lives in its own 4 KB partition, not in the app image, so it's
// chosen at flash time — no toolchain, works with the prebuilt image — and
// survives app reflashes. Written by `make flash` or `make flash-pwr`
// (tools/pwrcfg.py encodes it, and must match decode() below):
//
//     "PWR1" magic, then
//     enabled(1) button_pin(1) ps_on_pin(1) button_gnd_pin(1) sense_pin(1)
//     hold_ms(2) boot_timeout_ms(2) sense_low_mv(2) sense_high_mv(2)
//     led_pin(1) wake_pin(1)
//
// u16s little-endian; pins are GPIO numbers, 0xFF = not wired. An erased
// partition (no magic) leaves the feature off, so a fresh board or a plain
// `make flash` is inert until someone opts in. Fields only ever get
// APPENDED: a blob written before led_pin existed reads 0xFF (erased flash)
// there, i.e. not wired — both directions stay compatible. The four u16s in
// the middle are Tuning's wire form, byte for byte: they are this feature's
// flash-time DEFAULTS, and the values in force are g_tune below; wake_pin is
// a default the same way, with g_wake the pin in force.

static const uint16_t WIRE_LEN = 15;

struct Config
{
    bool enabled = false;
    int8_t buttonPin = -1; // all pins: GPIO number, -1 = not wired
    int8_t psOnPin = -1;
    int8_t buttonGndPin = -1;
    int8_t sensePin = -1;
    int8_t ledPin = -1;             // feedback LED: blinks while the button reads pressed
    int8_t wakePin = -1;            // wake input: active-HIGH pulse = power on (only) —
                                    // the flash-time default, g_wake is in force
    Tuning tune;                    // the flash-time defaults for the tunings

    bool decode(const uint8_t* p, uint16_t len)
    {
        if (len < WIRE_LEN)
            return false;

        auto pin = [](uint8_t b) -> int8_t
        { return (b == 0xFF || b >= GPIO_NUM_MAX) ? -1 : (int8_t)b; };

        enabled = p[0] != 0;
        buttonPin = pin(p[1]);
        psOnPin = pin(p[2]);
        buttonGndPin = pin(p[3]);
        sensePin = pin(p[4]);
        tune.decode(p + 5);
        ledPin = pin(p[13]);
        wakePin = pin(p[14]);
        return true;
    }
};

static Config g_cfg; // loaded once in start(), read-only after

static bool loadConfig(Config& c)
{
    uint8_t b[4 + WIRE_LEN];
    if (!cfgstore::partition("pwrcfg", b, sizeof b))
        return false;

    if (memcmp(b, "PWR1", 4) != 0)
        return false;

    return c.decode(b + 4, WIRE_LEN);
}

// ---- the tunings (Tuning, NVS, CMD_PWR_TUNING, the phone) ----

void Tuning::encode(uint8_t* p) const
{
    const uint16_t v[4] = {holdMs, bootTimeoutMs, senseLowMv, senseHighMv};
    for (int i = 0; i < 4; i++)
    {
        p[2 * i] = (uint8_t)v[i];
        p[2 * i + 1] = (uint8_t)(v[i] >> 8);
    }
}

void Tuning::decode(const uint8_t* p)
{
    auto u16 = [](const uint8_t* q) -> uint16_t
    { return (uint16_t)(q[0] | (q[1] << 8)); };
    holdMs = u16(p);
    bootTimeoutMs = u16(p + 2);
    senseLowMv = u16(p + 4);
    senseHighMv = u16(p + 6);
}

bool Tuning::valid(const char** why) const
{
    const char* r = nullptr;
    if (holdMs < 100)
        r = "hold under 0.1 s";
    else if (bootTimeoutMs < 1000)
        r = "boot timeout under 1 s";
    else if (senseLowMv >= senseHighMv)
        r = "sense_low_mv not below sense_high_mv";
    if (why)
        *why = r;
    return r == nullptr;
}

bool Tuning::operator==(const Tuning& o) const
{
    return holdMs == o.holdMs && bootTimeoutMs == o.bootTimeoutMs &&
           senseLowMv == o.senseLowMv && senseHighMv == o.senseHighMv;
}

// the tunings in force: the partition's until a saved override or a push
// says otherwise. Written only by the pwr task (in loop(), from the pending
// slot below, and in start()); read there on every poll and, as single
// aligned u16s, by snapshot() from the BLE task.
static Tuning g_tune;
static volatile uint32_t g_tuneSeq = 1; // bumps when g_tune changes

// a tuning staged by setTuning() (any task), consumed at the top of loop():
// the same one-consumer hand-off as the remote request, but eight bytes, so
// a critical section keeps a half-written one from being read
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_pendTune[proto::PWR_TUNING_LEN];
static volatile bool g_pendTuneSet = false;

// the wake input's pin in force: the partition's until a saved pick or a
// push says otherwise (the same layering as g_tune). Written only by the pwr
// task (start(), drainWake()); read there every poll and, as one aligned
// byte, by snapshot() and usesPin() from other tasks. A pick staged by
// setWakePin() (any task) sits in g_pendWake until the task consumes it —
// a single byte, so no critical section: -2 = nothing staged.
static int8_t g_wake = -1;
static volatile int8_t g_pendWake = -2;

// ---- state ----

// OFF: PS_ON# released. BOOTING: asserted, waiting for the sense line (only
// entered when a sense pin is wired). ON: asserted, board up (or no sense
// wire to say otherwise).
enum State
{
    OFF,
    BOOTING,
    ON
};

static const char* stateName(State s)
{
    return s == OFF ? "OFF" : s == BOOTING ? "BOOTING" : "ON";
}

static const uint32_t SAMPLE_LOG_MS = 1000; // min gap between periodic mv lines

static nvs_handle_t g_nvs = 0;
static State g_state = OFF;
static bool g_asserted = false;  // PS_ON# currently sunk low
static uint8_t g_savedOn = 0;    // NVS mirror of the intended PSU state
static uint32_t g_bootStart = 0; // millis() of the last power-on (BOOTING timeout)

// button debounce / press tracking. btnStable doubles as the failsafe flag:
// while it is set the sense line cannot power anything off.
static bool btnStable = false; // debounced "pressed"
static bool btnLastRaw = false;
static uint32_t btnLastChange = 0;
static uint32_t btnPressStart = 0;
static bool btnLongFired = false; // this press can no longer force off: either
                                  // it already did, or it is the press that
                                  // powered on / was already held when the
                                  // chip came up (holding to keep the failsafe
                                  // alive must never cut the power it protects)

// wake input debounce / arming. wakeArmed is set once the debounced input has
// read idle for WAKE_ARM_MS since the chip came up (or since the last time it
// dropped to idle before arming) — only then does a rising edge power on.
static bool wakeStable = false; // debounced "asserted"
static bool wakeLastRaw = false;
static uint32_t wakeLastChange = 0;
static bool wakeArmed = false;

// graceful-shutdown request in flight: millis() when we asked the host (0 =
// nothing outstanding), and when the "nobody answered" LED burst began
static uint32_t g_reqStart = 0;
static uint32_t g_nakBlink = 0;

// the feature made it through start()'s config checks and its task runs —
// what psuState() keys "-1 = off" on
static bool g_active = false;

// a remote request staged by remoteRequest() (BLE), consumed at the top of
// loop() so every power decision is made on this task, exactly like a button
// edge. Single aligned byte, one consumer; a second write superseding an
// unconsumed first is the semantics we want.
static volatile uint8_t g_remote = REMOTE_NONE;

// sense debounce (on the hysteresis output, not the raw voltage)
static adc_oneshot_unit_handle_t g_adc = nullptr;
static adc_cali_handle_t g_cali = nullptr;
static adc_channel_t g_chan;
static bool senseLevel = false; // hysteresis state
static bool senseStable = false;
static bool senseLastRaw = false;
static uint32_t senseChange = 0;
static uint32_t g_lastSampleMs = 0; // throttles the periodic mv log, and the
                                    // idle (state OFF) sample for the dashboard
static volatile uint16_t g_lastMv = 0xFFFF; // the last sense reading, for the
                                            // dashboard; 0xFFFF = none yet

// ---- PS_ON# line ----

// (re)configure the pin and stage `assert` glitch-free: the new level is set
// before any hold latched by a previous life is released, so a re-assert after
// a crash never lets the line float in between. The hold is re-latched while
// asserted so the level survives the next internal reset too. Chip caveats: a
// C3 can hold any output pin, a plain ESP32 only its RTC-capable ones (0, 2,
// 4, 12-15, 25-27, 32, 33 — elsewhere gpio_hold_en fails and only the NVS
// restore in start() protects); and no hold survives the EN-pin reset a plain
// ESP32 gets from esptool/dev-board auto-reset circuits — that's a genuine
// chip power cycle (reads as ESP_RST_POWERON, so start() won't restore
// either). Flashing a plain ESP32 therefore drops PS_ON#: do it with a
// bypass jumper in place. The C3's USB Serial/JTAG resets are internal and
// keep the hold.
static void applyPsOn(bool assert_)
{
    // The pin drives the gate of an N-channel MOSFET (2N7000: gate here,
    // source to GND, drain to PS_ON#), not PS_ON# directly. Gate HIGH turns
    // the MOSFET on and sinks PS_ON# to ground (PSU on); gate LOW turns it off
    // and the PSU's own 5 V pull-up carries PS_ON# high (PSU off). The
    // transistor is what makes this work at all: a 3.3 V pad can't sit on a
    // line pulled to 5 V — releasing it back-feeds the pad's clamp and never
    // reaches a clean 5 V "off" — so the MOSFET level-shifts and the pad only
    // ever sees a 3.3 V gate. A gate pull-down (~100 kΩ to ground) holds the
    // MOSFET off whenever the pad isn't driving: before start() runs, through
    // a reset, and on the PWR=off release path that leaves the pin an input.
    //
    // level BEFORE mode still, and now it's the safe direction too: a fresh
    // pad's output register is 0 = gate low = MOSFET off, so bring-up never
    // sinks PS_ON# — the old spurious power-on pulse can't happen.
    gpio_set_level((gpio_num_t)g_cfg.psOnPin, assert_ ? 1 : 0);

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << g_cfg.psOnPin;
    io.mode = GPIO_MODE_OUTPUT; // push-pull: we drive a gate, not the 5 V line
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    gpio_hold_dis((gpio_num_t)g_cfg.psOnPin);
    if (assert_)
        gpio_hold_en((gpio_num_t)g_cfg.psOnPin);
}

// Is the pad hold on PS_ON# still latched from a previous life? The hold bit
// is only ever set while asserted (applyPsOn), lives in the always-on domain,
// and a true power-on reset clears it — so it is a second, NVS-independent
// record of "the PSU is meant to be on", one that nothing writing the flash
// can erase. It matters because NVS can be legitimately empty at the very
// moment the machine's power hangs on this pin: `make clear-nvs`, or a `make
// flash` whose merged image paved over the nvs region (it did, before the
// Makefile learned to skip it). Trusting savedOn alone then made start()
// release PS_ON# and hard-cut the running host at the end of its own reflash.
// Only the C3 can answer: its single RTC_CNTL_DIG_PAD_HOLD_REG covers every
// pad (bit n = GPIO n). The plain ESP32's hold bits are scattered across the
// RTC-IO registers — and moot for the flashing case anyway, since esptool
// EN-resets that chip, which drops the hold and reads as ESP_RST_POWERON.
static bool psOnHeld()
{
#if CONFIG_IDF_TARGET_ESP32C3
    return g_cfg.psOnPin >= 0 &&
           REG_GET_BIT(RTC_CNTL_DIG_PAD_HOLD_REG, BIT(g_cfg.psOnPin)) != 0;
#else
    return false;
#endif
}

// remember the intended PSU state so start() can restore it after a reset;
// written only on actual on/off events, so NVS wear is a non-issue
static void setSavedOn(uint8_t on)
{
    if (on == g_savedOn)
        return;

    nvs_set_u8(g_nvs, "on", on);
    nvs_commit(g_nvs);
    g_savedOn = on;
}

static void psuAssert()
{
    applyPsOn(true);
    g_asserted = true;
    setSavedOn(1);
}

static void psuRelease()
{
    applyPsOn(false);
    g_asserted = false;
    setSavedOn(0);
}

// ---- board sense (ADC) ----

static void senseSetup()
{
    if (g_cfg.sensePin < 0)
        return;

    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(g_cfg.sensePin, &unit, &g_chan) != ESP_OK)
    {
        PLOG("sense: gpio%d is not ADC-capable; running without sense",
             g_cfg.sensePin);
        return; // not an ADC-capable pin; run without sense
    }

    adc_oneshot_unit_init_cfg_t uc = {};
    uc.unit_id = unit;
    if (adc_oneshot_new_unit(&uc, &g_adc) != ESP_OK)
    {
        PLOG("sense: gpio%d ADC unit init FAILED; running without sense",
             g_cfg.sensePin);
        g_adc = nullptr;
        return;
    }

    // 12 dB attenuation reads to ~3.1 V — covers TPMS1's ~2.9 V "up" level
    adc_oneshot_chan_cfg_t cc = {};
    cc.atten = ADC_ATTEN_DB_12;
    cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(g_adc, g_chan, &cc);

    // eFuse calibration for real millivolts where the chip has it; the raw
    // fallback in readSenseMv is plenty for a 2:1 hysteresis window
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id = unit;
    cal.chan = g_chan;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &g_cali) != ESP_OK)
        g_cali = nullptr;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal = {};
    cal.unit_id = unit;
    cal.atten = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&cal, &g_cali) != ESP_OK)
        g_cali = nullptr;
#endif

    PLOG("sense: gpio%d -> ADC%d ch%d init=OK cali=%s (low=%u high=%u mv)",
         g_cfg.sensePin, (int)unit + 1, (int)g_chan, g_cali ? "yes" : "raw",
         g_tune.senseLowMv, g_tune.senseHighMv);
}

static uint32_t readSenseMv()
{
    uint32_t acc = 0;

    for (int i = 0; i < SENSE_OVERSAMPLE; i++)
    {
        int raw = 0;
        adc_oneshot_read(g_adc, g_chan, &raw);

        int mv = 0;
        if (!g_cali || adc_cali_raw_to_voltage(g_cali, raw, &mv) != ESP_OK)
            mv = raw * 3100 / 4095; // uncalibrated: full scale ≈ 3.1 V at 12 dB

        acc += (uint32_t)mv;
    }

    return acc / SENSE_OVERSAMPLE;
}

// ---- state transitions ----

// bumped only here in powerOn() — the warm-reset re-hold in start() must NOT
// count (see powerOnSeq() in the header)
static volatile uint32_t g_powerOnSeq = 0;

static void powerOn(uint32_t now)
{
    psuAssert();
    g_bootStart = now;
    g_state = g_adc ? BOOTING : ON;
    g_powerOnSeq = g_powerOnSeq + 1; // no volatile++: deprecated in C++20
    PLOG("power ON: asserting PS_ON#, state=%s", stateName(g_state));

    // fresh sense tracking for this power cycle
    senseLevel = senseStable = senseLastRaw = false;
    senseChange = now;
}

static void powerOff()
{
    psuRelease();
    g_state = OFF;
}

// ask the host to shut itself down (see the short-press note at the top). Asking
// again while one is outstanding is deliberate — the user pressing a second time
// wants another try, and the daemon acts on the first request only. `why` names
// the gesture for the log: "short press" (the button) or "remote" (BLE).
static void requestShutdown(uint32_t now, const char* why)
{
    g_reqStart = now ? now : 1;
    hostreq::request(proto::REQ_HOST_SHUTDOWN);
    PLOG("%s: asked the host for a graceful shutdown", why);
}

// ---- the task ----

// the tunings pushed or dialled since the last poll: validated already
// (setTuning), so applying is a copy — and a persist when it is a change
static void drainTuning()
{
    if (!g_pendTuneSet)
        return;

    uint8_t b[proto::PWR_TUNING_LEN];
    taskENTER_CRITICAL(&g_mux);
    memcpy(b, g_pendTune, sizeof b);
    g_pendTuneSet = false;
    taskEXIT_CRITICAL(&g_mux);

    Tuning t;
    t.decode(b);
    if (t == g_tune)
        return;

    g_tune = t;
    g_tuneSeq = g_tuneSeq + 1;
    uint8_t base[proto::PWR_TUNING_LEN];
    g_cfg.tune.encode(base);
    cfgstore::save(g_nvs, "tune", "tunebase", b, base, sizeof b);
    PLOG("tuning set: hold=%u boottmo=%u sense low=%u high=%u mv", g_tune.holdMs,
         g_tune.bootTimeoutMs, g_tune.senseLowMv, g_tune.senseHighMv);
}

// ---- the wake input's pin ----

// the wire byte for a pin (the partition's, NVS's and CMD_PWR_WAKE's form)
static uint8_t wakeByte(int8_t pin) { return pin < 0 ? 0xFF : (uint8_t)pin; }

// (re)configure `pin` as the wake input — pull-down, so it reads idle with
// nothing (or an unpowered puck) on it — and start its debounce over, NOT
// armed: loop() waits for WAKE_ARM_MS of idle first, so a level already high
// here is never a press. A pin of -1 configures nothing.
static void wakeSetup(int8_t pin)
{
    wakeStable = wakeLastRaw = wakeArmed = false;
    wakeLastChange = millis();
    if (pin < 0)
        return;

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    wakeLastRaw = gpio_get_level((gpio_num_t)pin) == 1;
    if (wakeLastRaw)
        PLOG("wake: gpio%d already high -> waiting for it to settle idle", pin);
}

// let a pin the wake input no longer reads go: a plain floating input, the
// pad's reset state, so a sibling feature picking it up later starts clean
static void wakeRelease(int8_t pin)
{
    if (pin < 0)
        return;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
}

// can the wake input read on GPIO g? The flasher checks the configured pin
// against the chip's facts (tools/pwrcfg.py); this is the check for a pin
// that arrives at runtime, and the one place a phone's pick is refused with
// a reason. The fan controller's input check already knows everything that
// hurts an input here too (its own PWM outputs, the strip, the flash pads,
// the host link, the boot straps — and our pins, via usesPin, which is why
// the pin in force is accepted before asking). On top: a pin a fan header
// reads its PWM on, and the plain ESP32's input-only pads, which have no
// pull-down to make an unplugged puck read idle.
static bool wakePinOk(int g, const char** why)
{
    const char* r = nullptr;
    if (g < 0 || g == g_wake)
        r = nullptr;
    else if (!fan::inputPinFree(g, &r))
        ; // r says which
    else if (fan::readsPin(g))
        r = "a fan header's PWM input";
#if CONFIG_IDF_TARGET_ESP32
    else if (g >= 34)
        r = "an input-only pad with no pull-down";
#endif
    if (why)
        *why = r;
    return r == nullptr;
}

// a wake pin staged by setWakePin(): checked there, so applying is the pin
// swap — and a persist when it is a change
static void drainWake()
{
    int8_t pin = g_pendWake;
    if (pin == -2)
        return;
    g_pendWake = -2;
    if (pin == g_wake)
        return;

    wakeRelease(g_wake);
    g_wake = pin;
    wakeSetup(g_wake);
    g_tuneSeq = g_tuneSeq + 1;
    uint8_t v = wakeByte(g_wake), base = wakeByte(g_cfg.wakePin);
    cfgstore::save(g_nvs, "wake", "wakebase", &v, &base, 1);
    if (g_wake < 0)
        PLOG("wake input off");
    else
        PLOG("wake input on gpio%d", g_wake);
}

static void loop()
{
    uint32_t now = millis();

    drainTuning();
    drainWake();

    // a staged remote request first: the same two gestures as the button,
    // minus the hold/failsafe semantics (those belong to a finger on the real
    // switch). Anything that doesn't fit the current state is dropped with a
    // log line — the remote's status characteristic is how a client finds out.
    if (g_remote != REMOTE_NONE)
    {
        uint8_t r = g_remote;
        g_remote = REMOTE_NONE;

        if (r == REMOTE_ON && g_state == OFF)
        {
            PLOG("remote: power on");
            powerOn(now);
        }
        else if (r == REMOTE_OFF && g_state == ON)
            requestShutdown(now, "remote");
        else if (r == REMOTE_OFF_HARD && g_state != OFF)
        {
            // the remote hold: a hard cut, for a wedged (or never-booting)
            // machine. Same effect as holding the button for holdMs.
            PLOG("remote: power OFF (hard), releasing PS_ON#");
            powerOff();
        }
        else
            PLOG("remote: request %u ignored in state %s", (unsigned)r,
                 stateName(g_state));
    }

    if (g_wake >= 0)
    {
        // active HIGH — OpenPuck's PWR_SWITCH_ACTIVE default; the pull-down
        // makes "nothing connected" read idle
        bool raw = gpio_get_level((gpio_num_t)g_wake) == 1;

        if (raw != wakeLastRaw)
        {
            wakeLastRaw = raw;
            wakeLastChange = now;
        }

        if (raw != wakeStable && now - wakeLastChange >= DEBOUNCE_MS)
        {
            wakeStable = raw;

            if (wakeStable)
            {
                // the rising edge: power on, and only that. Every other
                // outcome is logged, because from the outside a dropped pulse
                // looks like a puck that didn't fire.
                if (!wakeArmed)
                    PLOG("wake: pulse before the input had settled idle, ignored");
                else if (g_state == OFF)
                {
                    PLOG("wake: power on");
                    powerOn(now);
                }
                else
                    PLOG("wake: pulse ignored in state %s", stateName(g_state));
            }
        }

        // arm once the raw level has sat idle for WAKE_ARM_MS — measured on
        // the raw input so a bounce during the window restarts it
        if (!wakeArmed && !raw && now - wakeLastChange >= WAKE_ARM_MS)
        {
            wakeArmed = true;
            PLOG("wake: input idle, armed");
        }
    }

    if (g_cfg.buttonPin >= 0)
    {
        bool raw = gpio_get_level((gpio_num_t)g_cfg.buttonPin) == 0;

        if (raw != btnLastRaw)
        {
            btnLastRaw = raw;
            btnLastChange = now;
        }

        if (raw != btnStable && now - btnLastChange >= DEBOUNCE_MS)
        {
            btnStable = raw;

            if (btnStable)
            {
                btnPressStart = now;
                btnLongFired = false;

                if (g_state == OFF)
                {
                    // press while off -> on, right here on the press edge, so
                    // that holding it afterwards means "failsafe" and not
                    // "power back off" — hence btnLongFired on the way in.
                    powerOn(now);
                    btnLongFired = true;
                }
            }
            else if (!btnLongFired)
            {
                // released early, and this press neither powered anything on nor
                // forced anything off (both set btnLongFired): the short press.
                if (g_state == ON)
                    requestShutdown(now, "short press");
                else
                    // BOOTING: the OS isn't up, so there's nobody to ask yet.
                    // Logged because from the outside this looks like a dead
                    // button.
                    PLOG("short press ignored: board is still BOOTING");
            }
        }

        // hold-to-force-off fires while still held (no release needed — the
        // user is telling us the machine is wedged), once per press
        if (btnStable && !btnLongFired && g_state != OFF &&
            now - btnPressStart >= g_tune.holdMs)
        {
            btnLongFired = true;
            PLOG("power OFF: button held %ums, releasing PS_ON#",
                 (unsigned)(now - btnPressStart));
            powerOff();
        }

        // feedback LED: blink while the debounced button reads pressed, and
        // again — faster, for NAK_BLINK_MS — when a shutdown request went
        // unanswered, which is the one outcome that leaves no other trace (the
        // machine simply stays on). A blink is visible on active-high and
        // active-low LEDs alike, so the board's polarity never needs
        // configuring; idle drives HIGH, which is dark on the common active-low
        // onboard LEDs.
        if (g_cfg.ledPin >= 0)
        {
            bool low = false;

            if (btnStable)
                low = (now - btnPressStart) / LED_BLINK_MS % 2 == 0;
            else if (wakeStable)
                // a wake pulse (300 ms from an OpenPuck) blinks the same way
                low = (now - wakeLastChange) / LED_BLINK_MS % 2 == 0;
            else if (g_nakBlink && now - g_nakBlink < NAK_BLINK_MS)
                low = (now - g_nakBlink) / NAK_BLINK_HALF_MS % 2 == 0;
            else
                g_nakBlink = 0;

            gpio_set_level((gpio_num_t)g_cfg.ledPin, low ? 0 : 1);
        }
    }

    // an outstanding graceful-shutdown request. Note what this does NOT do: it
    // never touches PS_ON# on any outcome. Answered means the OS is going down
    // and the follow-down will cut the PSU; unanswered means nobody could hear
    // us and the machine stays up, which is the safe end of the two.
    if (g_reqStart)
    {
        if (hostreq::acked())
        {
            PLOG("host accepted the shutdown; waiting for it to go down");
            hostreq::cancel();
            g_reqStart = 0;
        }
        else if (g_state == OFF)
        {
            // the power went away under us (a hold, or a shutdown already in
            // flight); nothing left to ask
            hostreq::cancel();
            g_reqStart = 0;
        }
        else if (now - g_reqStart >= REQ_ACK_MS)
        {
            PLOG("no answer in %ums: daemon down, or its "
                 "power_switch.short_press is null",
                 (unsigned)REQ_ACK_MS);
            hostreq::cancel();
            g_reqStart = 0;
            g_nakBlink = now ? now : 1;
        }
    }

    if (g_adc && g_state == OFF && now - g_lastSampleMs >= SAMPLE_LOG_MS)
    {
        // OFF isn't sensed (PS_ON# released means the rail is down by
        // construction), but the dashboard's calibration view wants to show
        // what the wire reads with the machine off too: one sample a second
        g_lastSampleMs = now;
        g_lastMv = (uint16_t)readSenseMv();
    }

    if (g_adc && g_state != OFF)
    {
        uint32_t mv = readSenseMv();
        g_lastMv = (uint16_t)mv;

        // throttled raw reading: the single most useful diagnostic — what the
        // sense line actually sits at. Gated on active() so it costs nothing
        // unless the host is draining the log.
        if (dbglog::active() && now - g_lastSampleMs >= SAMPLE_LOG_MS)
        {
            g_lastSampleMs = now;
            PLOG("sense mv=%u state=%s level=%d stable=%d%s", (unsigned)mv,
                 stateName(g_state), senseLevel, senseStable,
                 btnStable ? " HELD(failsafe)" : "");
        }

        // hysteresis on the averaged voltage...
        if (senseLevel)
            senseLevel = !(mv < g_tune.senseLowMv);
        else
            senseLevel = mv > g_tune.senseHighMv;

        // ...then a time debounce on top: quick to believe "up", slow to
        // believe "down" (brief dips happen during boot/reset)
        if (senseLevel != senseLastRaw)
        {
            senseLastRaw = senseLevel;
            senseChange = now;
            PLOG("sense level %s mv=%u (low=%u high=%u)",
                 senseLevel ? "up" : "down", (unsigned)mv, g_tune.senseLowMv,
                 g_tune.senseHighMv);
        }

        // FAILSAFE: while the button is physically held, nothing the sense line
        // says may release PS_ON#. Both timers are parked rather than their
        // actions merely skipped, so the failsafe leaves no residue: a level
        // that changed under the finger is still *pending* when it lifts and
        // gets a full debounce from there, and the boot timeout gets its full
        // window from the release instead of having expired mid-hold and
        // firing the instant contact breaks. The voltage above keeps tracking
        // and logging throughout — that log is the point of holding.
        if (btnStable)
        {
            senseChange = now;
            g_bootStart = now;
        }
        else
        {
            uint32_t need = senseLevel ? DEBOUNCE_MS : BOARD_OFF_DEBOUNCE_MS;

            if (senseLevel != senseStable && now - senseChange >= need)
            {
                senseStable = senseLevel;
                PLOG("sense STABLE %s (state=%s)", senseStable ? "up" : "down",
                     stateName(g_state));

                if (g_state == BOOTING && senseStable)
                {
                    g_state = ON;
                    PLOG("boot confirmed: sense up -> state=ON");
                }
                else if (g_state == ON && !senseStable)
                {
                    PLOG("power OFF: follow-down, board went away, releasing "
                         "PS_ON#");
                    powerOff(); // the board shut itself down; PSU follows it
                }
            }

            if (g_state == BOOTING && now - g_bootStart >= g_tune.bootTimeoutMs)
            {
                PLOG("power OFF: boot-timeout, sense never came up in %ums",
                     g_tune.bootTimeoutMs);
                powerOff(); // never came up; don't leave the PSU energized
            }
        }
    }
}

static void taskMain(void*)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        loop();
    }
}

// ---- public API ----

int senseState()
{
    if (!g_adc)
        return -1;

    return g_state != OFF && senseStable ? 1 : 0;
}

uint32_t powerOnSeq() { return g_powerOnSeq; }

int psuState()
{
    if (!g_active)
        return -1;

    return g_state == OFF ? 0 : g_state == BOOTING ? 1 : 2;
}

void remoteRequest(Remote r)
{
    if (g_active)
        g_remote = r;
}

bool usesPin(int gpio)
{
    if (!g_cfg.enabled || gpio < 0)
        return false;
    return gpio == g_cfg.buttonPin || gpio == g_cfg.psOnPin || gpio == g_cfg.buttonGndPin ||
           gpio == g_cfg.sensePin || gpio == g_cfg.ledPin || gpio == g_wake;
}

bool setWakePin(int gpio, const char** why)
{
    const char* r = nullptr;
    if (gpio == 0xFF)
        gpio = -1; // the wire's "none"
    if (!g_active)
        r = "power switch is off";
    else if (gpio >= GPIO_NUM_MAX)
        r = "not a GPIO on this chip";
    else
        wakePinOk(gpio, &r);
    if (why)
        *why = r;
    if (r)
        return false;

    g_pendWake = (int8_t)gpio;
    return true;
}

bool setTuning(const uint8_t* payload, uint16_t len, const char** why)
{
    const char* r = nullptr;
    Tuning t;
    if (!g_active)
        r = "power switch is off";
    else if (len < proto::PWR_TUNING_LEN)
        r = "short payload";
    else
    {
        t.decode(payload);
        t.valid(&r);
    }
    if (why)
        *why = r;
    if (r)
        return false;

    taskENTER_CRITICAL(&g_mux);
    memcpy(g_pendTune, payload, proto::PWR_TUNING_LEN);
    g_pendTuneSet = true;
    taskEXIT_CRITICAL(&g_mux);
    return true;
}

uint32_t settingsSeq() { return g_tuneSeq; }

void snapshot(Snapshot& s)
{
    // single aligned reads of values the pwr task writes, as psuState(); a
    // tuning straddling a change pairs one old value with a new one for one
    // dashboard poll, which is all the view is for
    s.active = g_active;
    if (!g_active)
        return;
    s.psOnPin = g_cfg.psOnPin;
    s.buttonPin = g_cfg.buttonPin;
    s.buttonGndPin = g_cfg.buttonGndPin;
    s.sensePin = g_adc ? g_cfg.sensePin : -1;
    s.ledPin = g_cfg.ledPin;
    s.wakePin = g_wake;
    s.psu = g_state == OFF ? 0 : g_state == BOOTING ? 1 : 2;
    s.senseMv = g_adc ? g_lastMv : 0xFFFF;
    s.tuning = g_tune;
}

void start()
{
    bool loaded = loadConfig(g_cfg);

    nvs_open("pwrsw", NVS_READWRITE, &g_nvs);
    nvs_get_u8(g_nvs, "on", &g_savedOn);

    // the tunings: the partition's, unless a push or a dial was saved over
    // exactly these partition values (cfgstore.hpp — a re-flash wins)
    g_tune = g_cfg.tune;
    {
        uint8_t base[proto::PWR_TUNING_LEN], saved[proto::PWR_TUNING_LEN];
        g_cfg.tune.encode(base);
        if (cfgstore::load(g_nvs, "tune", "tunebase", base, sizeof base, saved))
        {
            Tuning t;
            t.decode(saved);
            if (t.valid())
                g_tune = t;
        }
    }
    // the wake pin the same way. A saved pick this build can't read on
    // (the fan headers or the strip moved onto it since) falls back to the
    // partition's, and the debug log says so
    g_wake = g_cfg.wakePin;
    {
        uint8_t base = wakeByte(g_cfg.wakePin), saved;
        if (cfgstore::load(g_nvs, "wake", "wakebase", &base, 1, &saved))
        {
            int pin = saved == 0xFF ? -1 : saved;
            const char* why = nullptr;
            if (pin < GPIO_NUM_MAX && wakePinOk(pin, &why))
                g_wake = (int8_t)pin;
            else
                PLOG("saved wake pin gpio%d refused — %s; running the partition's", pin,
                     why ? why : "not a GPIO on this chip");
        }
    }

    if (!loaded || !g_cfg.enabled || g_cfg.psOnPin < 0)
    {
        // feature off. If it was just disabled by a reflash (PWR=off) while
        // PS_ON# was held low, that hold is still latched in the always-on
        // domain — release it (this cuts the board's power: turning the
        // feature off means the jumper goes back in) and clear the intent so
        // a later PWR=on doesn't resurrect it. Checked against the hold bit
        // itself as well as NVS: a reflash that wiped NVS alongside must not
        // leave the latch quietly energizing a PSU the config says to let go.
        if (g_savedOn || psOnHeld())
        {
            if (loaded && g_cfg.psOnPin >= 0)
                gpio_hold_dis((gpio_num_t)g_cfg.psOnPin);
            setSavedOn(0);
        }
        return;
    }

    PLOG("cfg button=%d gnd=%d ps_on=%d sense=%d led=%d wake=%d%s hold=%u boottmo=%u%s",
         g_cfg.buttonPin, g_cfg.buttonGndPin, g_cfg.psOnPin, g_cfg.sensePin,
         g_cfg.ledPin, g_wake, g_wake == g_cfg.wakePin ? "" : " (from NVS, over the partition's)",
         g_tune.holdMs, g_tune.bootTimeoutMs,
         g_tune == g_cfg.tune ? "" : " (tuning from NVS, over the partition's)");
    bool held = psOnHeld();
    PLOG("start: reset=%d savedOn=%d held=%d", (int)esp_reset_reason(),
         (int)g_savedOn, (int)held);

    if ((g_savedOn || held) && esp_reset_reason() != ESP_RST_POWERON)
    {
        // the chip reset while holding PS_ON# low (crash, watchdog, reflash):
        // put it back before anything slower runs. If the sense wire confirms
        // the board is (still) up, BOOTING collapses to ON within one debounce.
        // A latched hold counts as intent even when NVS says nothing (see
        // psOnHeld) — and setSavedOn rebuilds the NVS record it carried.
        g_asserted = true;
        g_state = g_cfg.sensePin >= 0 ? BOOTING : ON;
        g_bootStart = millis();
        setSavedOn(1);
    }
    else if (g_savedOn)
    {
        // true power-on reset: 5VSB itself was lost, so everything is off and
        // stays off until a button press. Clear the stale intent.
        setSavedOn(0);
    }

    // PS_ON# first (glitch-free, see applyPsOn), then the slower pins
    applyPsOn(g_asserted);

    if (g_cfg.buttonPin >= 0)
    {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << g_cfg.buttonPin;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE; // pressed = LOW
        gpio_config(&io);
        btnLastChange = millis();

        // A button that already reads pressed here is a press in progress
        // across the reset — the recovery case this feature exists for is
        // reflashing while holding the failsafe down, and the reset in the
        // middle of it must not turn that hold into a fresh press that forces
        // the power off holdMs later. Adopt it as an already-fired press: the
        // failsafe keeps applying (btnStable), forcing off does not. A false
        // read from a pull-up that hasn't settled is harmless — it only ever
        // errs towards keeping the power on, and clears within one debounce.
        if (gpio_get_level((gpio_num_t)g_cfg.buttonPin) == 0)
        {
            btnStable = btnLastRaw = btnLongFired = true;
            btnPressStart = btnLastChange;
            PLOG("start: button already held -> failsafe, no force-off");
        }
    }

    if (g_cfg.buttonGndPin >= 0)
    {
        // the button's local ground: plain push-pull LOW
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << g_cfg.buttonGndPin;
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        gpio_set_level((gpio_num_t)g_cfg.buttonGndPin, 0);
    }

    wakeSetup(g_wake); // the wake input, if any (see wakeSetup)

    if (g_cfg.ledPin >= 0)
    {
        gpio_set_level((gpio_num_t)g_cfg.ledPin, 1); // level before mode, as with PS_ON#
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << g_cfg.ledPin;
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
    }

    senseSetup();

    // a restored BOOTING with no working sense would strand the timeout
    if (g_state == BOOTING && !g_adc)
        g_state = ON;

    // priority above the idle/main tasks but below the LED service's 5: a
    // 10 ms button poll never needs to win against the strip's latch cadence
    g_active = true;
    xTaskCreate(taskMain, "pwr_sw", 4096, nullptr, 2, nullptr);
}
} // namespace pwr
