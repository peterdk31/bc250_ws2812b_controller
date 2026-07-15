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

#include "util.hpp"

// The control model (after Thunkar/bc250-esp32-switch, minus its WiFi/BLE):
//
//   * PS_ON# is active LOW and pulled up to 5 V inside the PSU, so the pin is
//     driven OPEN-DRAIN: sink to ground = PSU on, high-Z = PSU off. We never
//     push 3.3 V against the PSU's pull-up.
//   * The button reads with an internal pull-up (pressed = LOW); an optional
//     second pin is driven LOW as the button's local ground, so a two-wire
//     switch needs no run to a real GND pin.
//   * The sense wire (TPMS1 pin 9 on the BC-250) sits near ~2.9 V when the
//     board is up — high-impedance and hovering close to the digital logic
//     threshold, so it's read as an averaged ADC voltage with hysteresis
//     instead of a flaky digitalRead.
//   * "Off" here means cutting the PSU — a hard power-off, not a graceful OS
//     shutdown; hence the hold-to-fire threshold. A graceful shutdown is the
//     board's own: it turns itself off, the sense line drops, and we release
//     PS_ON# so the PSU follows it down.
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
static const uint32_t BOARD_OFF_DEBOUNCE_MS = 1500; // sense must stay down this long
                                                    // (filters dips during boot/reset)
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
//
// u16s little-endian; pins are GPIO numbers, 0xFF = not wired. An erased
// partition (no magic) leaves the feature off, so a fresh board or a plain
// `make flash` is inert until someone opts in with PWR=on.

static const uint16_t WIRE_LEN = 13;

struct Config
{
    bool enabled = false;
    int8_t buttonPin = -1; // all pins: GPIO number, -1 = not wired
    int8_t psOnPin = -1;
    int8_t buttonGndPin = -1;
    int8_t sensePin = -1;
    uint16_t holdMs = 5000;         // hold the button this long to force off
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

static nvs_handle_t g_nvs = 0;
static State g_state = OFF;
static bool g_asserted = false;  // PS_ON# currently sunk low
static uint8_t g_savedOn = 0;    // NVS mirror of the intended PSU state
static uint32_t g_bootStart = 0; // millis() of the last power-on (BOOTING timeout)

// button debounce / press tracking
static bool btnStable = false; // debounced "pressed"
static bool btnLastRaw = false;
static uint32_t btnLastChange = 0;
static uint32_t btnPressStart = 0;
static bool btnPressStartedOff = false; // press began while OFF (that press may
                                        // power on at release, never force off)
static bool btnLongFired = false;       // this press already forced off

// sense debounce (on the hysteresis output, not the raw voltage)
static adc_oneshot_unit_handle_t g_adc = nullptr;
static adc_cali_handle_t g_cali = nullptr;
static adc_channel_t g_chan;
static bool senseLevel = false; // hysteresis state
static bool senseStable = false;
static bool senseLastRaw = false;
static uint32_t senseChange = 0;

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
    // level BEFORE mode: a fresh pad's output register is 0, so configuring
    // the pin as an output first would sink PS_ON# low for a moment on every
    // boot — a spurious power-on pulse
    gpio_set_level((gpio_num_t)g_cfg.psOnPin, assert_ ? 0 : 1);

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << g_cfg.psOnPin;
    io.mode = GPIO_MODE_OUTPUT_OD;
    io.pull_up_en = GPIO_PULLUP_DISABLE; // the PSU's own 5 V pull-up owns the idle level
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
        return; // not an ADC-capable pin; run without sense

    adc_oneshot_unit_init_cfg_t uc = {};
    uc.unit_id = unit;
    if (adc_oneshot_new_unit(&uc, &g_adc) != ESP_OK)
    {
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

    // fresh sense tracking for this power cycle
    senseLevel = senseStable = senseLastRaw = false;
    senseChange = now;
}

static void powerOff()
{
    psuRelease();
    g_state = OFF;
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
                btnPressStartedOff = (g_state == OFF);
                btnLongFired = false;
            }
            else if (btnPressStartedOff && g_state == OFF &&
                     now - btnPressStart < g_cfg.holdMs)
            {
                // tap while off -> on. A press held past holdMs from OFF does
                // nothing at all (that gesture is reserved for forcing off).
                powerOn(now);
            }
        }

        // hold-to-force-off fires while still held (no release needed — the
        // user is telling us the machine is wedged), once per press
        if (btnStable && !btnLongFired && g_state != OFF &&
            now - btnPressStart >= g_cfg.holdMs)
        {
            btnLongFired = true;
            powerOff();
        }
    }

    if (g_adc && g_state != OFF)
    {
        uint32_t mv = readSenseMv();

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
        }

        uint32_t need = senseLevel ? DEBOUNCE_MS : BOARD_OFF_DEBOUNCE_MS;

        if (senseLevel != senseStable && now - senseChange >= need)
        {
            senseStable = senseLevel;

            if (g_state == BOOTING && senseStable)
                g_state = ON;
            else if (g_state == ON && !senseStable)
                powerOff(); // the board shut itself down; the PSU follows it
        }

        if (g_state == BOOTING && now - g_bootStart >= g_cfg.bootTimeoutMs)
            powerOff(); // never came up; don't leave the PSU energized
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

    senseSetup();

    // a restored BOOTING with no working sense would strand the timeout
    if (g_state == BOOTING && !g_adc)
        g_state = ON;

    // priority above the idle/main tasks but below the LED service's 5: a
    // 10 ms button poll never needs to win against the strip's latch cadence
    xTaskCreate(taskMain, "pwr_sw", 4096, nullptr, 2, nullptr);
}
} // namespace pwr
