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
static const int MAX_FANS = 6;           // LEDC channels on the C3

// how long the USB SOF keepalives must be gone before "present again" counts
// as the host coming back up — the same window led_service uses: bus resets
// during re-enumeration silence SOF for ~1 s, and those are not power cycles
static const uint32_t HOST_GONE_MS = 3000;

// ---- configuration (the `fancfg` flash partition) ----

// Same scheme as the power switch's `pwrcfg` (see power_switch.cpp): wiring
// in its own 4 KB partition, written at flash time by `make flash FAN=on ...`
// or `make flash-fan` (tools/fancfg.py encodes it, and must match decode()
// below):
//
//     "FAN1" magic, then
//     enabled(1) pin[6] duty[6] boost_duty(1) boost_secs(1)
//
// Pins are GPIO numbers, 0xFF = channel not wired; the wired channels, in
// order, are what CMD_FAN_DUTY's percents map to. Duties are percent, >100
// clamps to 100 — so erased flash in an appended field's place reads as a
// sane full-speed value. An erased partition (no magic) leaves the feature
// off. Fields only ever get APPENDED, as with pwrcfg.

static const uint16_t WIRE_LEN = 15;

struct Config
{
    bool enabled = false;
    int8_t pin[MAX_FANS] = {-1, -1, -1, -1, -1, -1};
    uint8_t duty[MAX_FANS] = {100, 100, 100, 100, 100, 100};
    uint8_t boostDuty = 100; // every channel runs at this...
    uint8_t boostSecs = 5;   // ...for this long after the host rail comes up

    bool decode(const uint8_t* p, uint16_t len)
    {
        if (len < WIRE_LEN)
            return false;

        auto pinOf = [](uint8_t b) -> int8_t
        { return (b == 0xFF || b >= GPIO_NUM_MAX) ? -1 : (int8_t)b; };
        auto pct = [](uint8_t b) -> uint8_t { return b > 100 ? 100 : b; };

        enabled = p[0] != 0;
        for (int i = 0; i < MAX_FANS; i++)
        {
            pin[i] = pinOf(p[1 + i]);
            duty[i] = pct(p[7 + i]);
        }
        boostDuty = pct(p[13]);
        boostSecs = p[14];
        return true;
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

    uint8_t b[4 + WIRE_LEN];
    if (esp_partition_read(part, 0, b, sizeof b) != ESP_OK)
        return false;

    if (memcmp(b, "FAN1", 4) != 0)
        return false;

    return c.decode(b + 4, WIRE_LEN);
}

// ---- state ----

static nvs_handle_t g_nvs = 0;
static int g_count = 0;            // wired channels; channel i = LEDC channel i
static int8_t g_pin[MAX_FANS];     // their GPIOs, in wire order
static uint8_t g_target[MAX_FANS]; // resting duty percents (flash defaults,
                                   // overridden by a persisted host push)
static bool g_started = false;     // gate for setDuty; set before the led_rx
                                   // task exists, so never raced

// host-pushed duty in flight from the led_rx task to this one — the whole
// array moves under one short critical section, the hostreq pattern
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_pending[MAX_FANS];
static uint8_t g_pendingCount = 0; // 0 = nothing pending

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

// push every channel's current duty (boost overrides all targets) to LEDC
static void applyAll()
{
    for (int i = 0; i < g_count; i++)
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i,
                      dutyOf(g_boosting ? g_cfg.boostDuty : g_target[i]));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
}

// ---- NVS ----

// The host-pushed duties persist so a daemon-less boot still runs at the
// last configured speed. Alongside them, the flash defaults they were saved
// under: a re-flashed fancfg with different duties is the user re-deciding,
// and outranks a stale push — the same newer-default-wins rule the LED
// service applies to its saved baud.
static void loadSaved()
{
    uint8_t saved[MAX_FANS], base[MAX_FANS];
    size_t n = sizeof saved;
    if (nvs_get_blob(g_nvs, "duty", saved, &n) != ESP_OK || n != sizeof saved)
        return;
    n = sizeof base;
    if (nvs_get_blob(g_nvs, "base", base, &n) != ESP_OK || n != sizeof base)
        return;
    if (memcmp(base, g_cfg.duty, sizeof base) != 0)
        return; // fancfg was re-flashed with new defaults since this was saved

    for (int i = 0; i < MAX_FANS; i++)
        g_target[i] = saved[i] > 100 ? 100 : saved[i];
}

static void persist()
{
    nvs_set_blob(g_nvs, "duty", g_target, MAX_FANS);
    nvs_set_blob(g_nvs, "base", g_cfg.duty, MAX_FANS);
    nvs_commit(g_nvs);
}

// ---- the task ----

// drain a host push, if one landed: update the resting targets and persist
// them (writes only on change, so a daemon restarting with the same config
// costs no flash wear). During a boost the new targets are stored but the
// fans stay at boostDuty until it expires.
static void drainPending()
{
    uint8_t buf[MAX_FANS];
    uint8_t n;

    taskENTER_CRITICAL(&g_mux);
    n = g_pendingCount;
    if (n)
        memcpy(buf, g_pending, sizeof buf);
    g_pendingCount = 0;
    taskEXIT_CRITICAL(&g_mux);

    if (!n)
        return;

    bool changed = false;
    for (int i = 0; i < g_count && i < n; i++)
        if (g_target[i] != buf[i])
        {
            g_target[i] = buf[i];
            changed = true;
        }

    if (!changed)
        return;

    persist();
    FLOG("host set duty %u/%u/%u/%u/%u/%u%%%s", g_target[0], g_target[1],
         g_target[2], g_target[3], g_target[4], g_target[5],
         g_boosting ? " (applies after the boost)" : "");

    if (!g_boosting)
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

    if (fire && g_cfg.boostSecs)
    {
        g_boosting = true;
        g_boostStart = now;
        FLOG("boost: host powered on — all fans to %u%% for %us (source=%s)",
             g_cfg.boostDuty, g_cfg.boostSecs, s >= 0 ? "sense" : "usb");
        applyAll();
    }

    if (g_boosting && now - g_boostStart >= (uint32_t)g_cfg.boostSecs * 1000)
    {
        g_boosting = false;
        FLOG("boost done — settling to %u/%u/%u/%u/%u/%u%%", g_target[0],
             g_target[1], g_target[2], g_target[3], g_target[4], g_target[5]);
        applyAll();
    }
}

static void taskMain(void*)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        uint32_t now = millis();
        drainPending();
        boostCheck(now);
    }
}

// ---- public API ----

void setDuty(const uint8_t* payload, uint16_t len)
{
    if (!g_started || len < 1)
        return;

    uint8_t n = payload[0];
    if (len < (uint16_t)(1 + n))
        return;
    if (n > MAX_FANS)
        n = MAX_FANS;

    taskENTER_CRITICAL(&g_mux);
    for (int i = 0; i < n; i++)
        g_pending[i] = payload[1 + i] > 100 ? 100 : payload[1 + i];
    g_pendingCount = n;
    taskEXIT_CRITICAL(&g_mux);
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

    for (int i = 0; i < MAX_FANS; i++)
        if (g_cfg.pin[i] >= 0)
        {
            g_pin[g_count] = g_cfg.pin[i];
            g_target[g_count] = g_cfg.duty[i];
            g_count++;
        }

    if (g_count == 0)
    {
        FLOG("enabled but no wired pins — feature off");
        return;
    }

    nvs_open("fan", NVS_READWRITE, &g_nvs);
    loadSaved();

    // one timer at the fan frequency, one channel per wired pin. All LEDC on
    // the C3 is the one low-speed group; the strip's RMT is a different
    // peripheral entirely, so the two never contend.
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

    for (int i = 0; i < g_count; i++)
    {
        ledc_channel_config_t cc = {};
        cc.gpio_num = g_pin[i];
        cc.speed_mode = LEDC_LOW_SPEED_MODE;
        cc.channel = (ledc_channel_t)i;
        cc.timer_sel = LEDC_TIMER_0;
        cc.duty = dutyOf(g_target[i]);
        cc.hpoint = 0;
        if (ledc_channel_config(&cc) != ESP_OK)
            FLOG("channel %d (gpio%d) init FAILED", i, g_pin[i]);
    }

    FLOG("cfg %d fans, pins %d/%d/%d/%d/%d/%d, duty %u/%u/%u/%u/%u/%u%%, "
         "boost %u%% %us",
         g_count, g_cfg.pin[0], g_cfg.pin[1], g_cfg.pin[2], g_cfg.pin[3],
         g_cfg.pin[4], g_cfg.pin[5], g_target[0], g_target[1], g_target[2],
         g_target[3], g_target[4], g_target[5], g_cfg.boostDuty,
         g_cfg.boostSecs);

    g_started = true;

    // priority 2 like the power switch: a 100 ms duty tick never needs to win
    // against the LED service's latch cadence
    xTaskCreate(taskMain, "fan", 4096, nullptr, 2, nullptr);
}
} // namespace fan
