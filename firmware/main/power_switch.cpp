#include "power_switch.hpp"

#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"

#include "protocol.hpp"

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
//     command (hostreq.hpp, and "sinks.serial.power_button" on the host). We
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
// it the line even rides through the reset itself. A true power-on reset
// (5VSB was lost) clears the intent instead: everything genuinely lost power,
// and the board should not boot just because standby came back.
namespace pwr
{
// ---- fixed tuning (values proven on the BC-250 by the reference project) ----

static const uint32_t POLL_MS = 10;             // task cadence; all debounces count in these
static const uint32_t DEBOUNCE_MS = 30;         // button, and sense going UP
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
// survives app reflashes. Written by `make flash PWR=on ...` or `make
// flash-pwr` (tools/pwrcfg.py encodes it, and must match decode() below):
//
//     "PWR1" magic, then
//     enabled(1) button_pin(1) ps_on_pin(1) button_gnd_pin(1) sense_pin(1)
//     hold_ms(2) boot_timeout_ms(2) sense_low_mv(2) sense_high_mv(2)
//     led_pin(1)
//
// u16s little-endian; pins are GPIO numbers, 0xFF = not wired. An erased
// partition (no magic) leaves the feature off, so a fresh board or a plain
// `make flash` is inert until someone opts in with PWR=on. Fields only ever
// get APPENDED: a blob written before led_pin existed reads 0xFF (erased
// flash) there, i.e. not wired — both directions stay compatible.

static const uint16_t WIRE_LEN = 14;

struct Config
{
    bool enabled = false;
    int8_t buttonPin = -1; // all pins: GPIO number, -1 = not wired
    int8_t psOnPin = -1;
    int8_t buttonGndPin = -1;
    int8_t sensePin = -1;
    int8_t ledPin = -1;             // feedback LED: blinks while the button reads pressed
    uint16_t holdMs = 2000;         // hold the button this long to force off
    uint16_t bootTimeoutMs = 10000; // sense never came up after power-on -> release
    uint16_t senseLowMv = 800;      // hysteresis: below = board down...
    uint16_t senseHighMv = 2000;    // ...above = board up, between = hold state

    bool decode(const uint8_t* p, uint16_t len)
    {
        if (len < WIRE_LEN)
            return false;

        auto pin = [](uint8_t b) -> int8_t
        { return (b == 0xFF || b >= GPIO_NUM_MAX) ? -1 : (int8_t)b; };
        auto u16 = [](const uint8_t* q) -> uint16_t
        { return (uint16_t)(q[0] | (q[1] << 8)); };

        enabled = p[0] != 0;
        buttonPin = pin(p[1]);
        psOnPin = pin(p[2]);
        buttonGndPin = pin(p[3]);
        sensePin = pin(p[4]);
        holdMs = u16(p + 5);
        bootTimeoutMs = u16(p + 7);
        senseLowMv = u16(p + 9);
        senseHighMv = u16(p + 11);
        ledPin = pin(p[13]);
        return true;
    }
};

static Config g_cfg; // loaded once in start(), read-only after

static bool loadConfig(Config& c)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "pwrcfg");
    if (!part)
        return false;

    uint8_t b[4 + WIRE_LEN];
    if (esp_partition_read(part, 0, b, sizeof b) != ESP_OK)
        return false;

    if (memcmp(b, "PWR1", 4) != 0)
        return false;

    return c.decode(b + 4, WIRE_LEN);
}

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

// graceful-shutdown request in flight: millis() when we asked the host (0 =
// nothing outstanding), and when the "nobody answered" LED burst began
static uint32_t g_reqStart = 0;
static uint32_t g_nakBlink = 0;

// sense debounce (on the hysteresis output, not the raw voltage)
static adc_oneshot_unit_handle_t g_adc = nullptr;
static adc_cali_handle_t g_cali = nullptr;
static adc_channel_t g_chan;
static bool senseLevel = false; // hysteresis state
static bool senseStable = false;
static bool senseLastRaw = false;
static uint32_t senseChange = 0;
static uint32_t g_lastSampleMs = 0; // throttles the periodic mv log

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
         g_cfg.senseLowMv, g_cfg.senseHighMv);
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

static void powerOn(uint32_t now)
{
    psuAssert();
    g_bootStart = now;
    g_state = g_adc ? BOOTING : ON;
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
// wants another try, and the daemon acts on the first request only.
static void requestShutdown(uint32_t now)
{
    g_reqStart = now ? now : 1;
    hostreq::request(proto::REQ_HOST_SHUTDOWN);
    PLOG("short press: asked the host for a graceful shutdown");
}

// ---- the task ----

static void loop()
{
    uint32_t now = millis();

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
                    requestShutdown(now);
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
            now - btnPressStart >= g_cfg.holdMs)
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
                 "sinks.serial.power_button is off",
                 (unsigned)REQ_ACK_MS);
            hostreq::cancel();
            g_reqStart = 0;
            g_nakBlink = now ? now : 1;
        }
    }

    if (g_adc && g_state != OFF)
    {
        uint32_t mv = readSenseMv();

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
            senseLevel = !(mv < g_cfg.senseLowMv);
        else
            senseLevel = mv > g_cfg.senseHighMv;

        // ...then a time debounce on top: quick to believe "up", slow to
        // believe "down" (brief dips happen during boot/reset)
        if (senseLevel != senseLastRaw)
        {
            senseLastRaw = senseLevel;
            senseChange = now;
            PLOG("sense level %s mv=%u (low=%u high=%u)",
                 senseLevel ? "up" : "down", (unsigned)mv, g_cfg.senseLowMv,
                 g_cfg.senseHighMv);
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

            if (g_state == BOOTING && now - g_bootStart >= g_cfg.bootTimeoutMs)
            {
                PLOG("power OFF: boot-timeout, sense never came up in %ums",
                     g_cfg.bootTimeoutMs);
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

void start()
{
    bool loaded = loadConfig(g_cfg);

    nvs_open("pwrsw", NVS_READWRITE, &g_nvs);
    nvs_get_u8(g_nvs, "on", &g_savedOn);

    if (!loaded || !g_cfg.enabled || g_cfg.psOnPin < 0)
    {
        // feature off. If it was just disabled by a reflash (PWR=off) while
        // PS_ON# was held low, that hold is still latched in the always-on
        // domain — release it (this cuts the board's power: turning the
        // feature off means the jumper goes back in) and clear the intent so
        // a later PWR=on doesn't resurrect it.
        if (g_savedOn)
        {
            if (loaded && g_cfg.psOnPin >= 0)
                gpio_hold_dis((gpio_num_t)g_cfg.psOnPin);
            setSavedOn(0);
        }
        return;
    }

    PLOG("cfg button=%d gnd=%d ps_on=%d sense=%d led=%d hold=%u boottmo=%u",
         g_cfg.buttonPin, g_cfg.buttonGndPin, g_cfg.psOnPin, g_cfg.sensePin,
         g_cfg.ledPin, g_cfg.holdMs, g_cfg.bootTimeoutMs);
    PLOG("start: reset=%d savedOn=%d", (int)esp_reset_reason(), (int)g_savedOn);

    if (g_savedOn && esp_reset_reason() != ESP_RST_POWERON)
    {
        // the chip reset while holding PS_ON# low (crash, watchdog, reflash):
        // put it back before anything slower runs. If the sense wire confirms
        // the board is (still) up, BOOTING collapses to ON within one debounce.
        g_asserted = true;
        g_state = g_cfg.sensePin >= 0 ? BOOTING : ON;
        g_bootStart = millis();
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
    xTaskCreate(taskMain, "pwr_sw", 4096, nullptr, 2, nullptr);
}
} // namespace pwr
