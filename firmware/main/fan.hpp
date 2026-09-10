#pragma once

#include <cstdint>

#include "protocol.hpp"

// PWM fan controller: drives up to six 4-pin PC fans (25 kHz LEDC, the fan
// spec's frequency), one per "header", from GPIOs chosen at flash time —
// standalone, like the power switch: it needs no daemon. The fans are powered
// externally (grounds common with this board); only their PWM inputs land
// here, so an unpowered fan just ignores the signal.
//
// Each header's duty comes from three places, strongest first:
//
//  - the boot boost: the header's own boost duty for boostSecs after the host
//    powers on (an AIO pump primes reliably at full speed; a header with no
//    boost sits it out). Armed by the power switch's power-on event and fired
//    when its sense wire confirms the rail, so a warm reset of this chip never
//    re-fires it — see boostCheck.
//  - the live duty: what the daemon's fan curves want right now
//    (CMD_FAN_LIVE). Volatile — never persisted — and it expires when the
//    daemon stops refreshing it, so a dead daemon can't leave a fan pinned low
//    under load. After the host announces that the machine is powering off
//    it is held instead: the fans wind down from where they are.
//  - the standalone settings: the resting ("fallback") duty and the boost,
//    per header, plus the boost length. Flash-time defaults come from the
//    `fancfg` partition (`make flash` / `make flash-fan` write it from the
//    daemon config's "fans" block); the daemon pushes the same values once at
//    startup (CMD_FAN_STANDALONE) and they persist in this module's own NVS
//    namespace, so an edited config applies on daemon-less boots too.
//
// Runs as its own task and owns its own NVS namespace; it knows nothing of
// the LED service. The LED task hands it the two fan commands and one word —
// "the host is shutting down" — and that is their whole acquaintance.
namespace fan
{
// bring the feature up and start its task. Called from app_main after
// pwr::start() — it reads pwr::senseState(), whose ADC is set up there — and
// before the slower filesystem mount, so the fans reach their duty early.
void start();

// a CMD_FAN_STANDALONE payload from the host (see protocol.hpp). Called on the
// led_rx task; hands the values to the fan task under a critical section. A
// safe no-op while the feature is off.
void setStandalone(const uint8_t* payload, uint16_t len);

// a CMD_FAN_LIVE payload from the host (see protocol.hpp). Same calling rules.
void setLive(const uint8_t* payload, uint16_t len);

// the host sent CMD_SHUTDOWN flagged as a power-off: hold the live duties
// through the power-down instead of letting them expire to the fallback.
void hostShutdown();

// ---- the BLE dashboard's view (ble.cpp) ----
//
// This module relays two host payloads it never reads — JSON text the daemon
// writes for the phone (protocol.hpp): its fan config (CMD_FAN_CONFIG, up to
// HOST_CONFIG_MAX bytes) and its telemetry (CMD_FAN_TELEM, up to
// HOST_TELEM_MAX) — each kept as the last one received, for ble.cpp to serve
// verbatim. The setters run on the led_rx task; the getters copy out under the
// lock from any task. They work whether or not the PWM feature itself is on —
// a box with the fans wired elsewhere still gets the daemon's readings on its
// phone.
static const uint16_t HOST_CONFIG_MAX = 512; // a GATT attribute's ceiling
static const uint16_t HOST_TELEM_MAX = 384;

void setHostConfig(const uint8_t* payload, uint16_t len);
void setHostTelemetry(const uint8_t* payload, uint16_t len);

// copy the last payload into out (max bytes); returns its length, 0 when none
// has arrived (or out is too small). *seq counts arrivals, so a poller can
// tell a new one from the same one; *ageMs is how long ago the last arrived
// (0xFFFFFFFF = never). Both optional.
uint16_t hostConfig(uint8_t* out, uint16_t max, uint32_t* seq = nullptr);
uint16_t hostTelemetry(uint8_t* out, uint16_t max, uint32_t* seq = nullptr,
                       uint32_t* ageMs = nullptr);

// what this module is doing right now, per header — the dashboard's local
// half, which is there even with the daemon down
struct Snapshot
{
    bool active = false;   // the PWM feature is on (some header wired)
    bool boosting = false; // the boot boost window is running
    bool hold = false;     // the host is powering off, live duties held
    bool live = false;     // the daemon's live duties are in force
    uint8_t wired[proto::FAN_CHANNELS];
    uint8_t duty[proto::FAN_CHANNELS];   // the duty actually applied
    uint8_t source[proto::FAN_CHANNELS]; // SRC_*: where that duty came from
};
static const uint8_t SRC_NONE = 0;     // header not wired
static const uint8_t SRC_FALLBACK = 1;
static const uint8_t SRC_LIVE = 2;
static const uint8_t SRC_BOOST = 3;
void snapshot(Snapshot& s);
} // namespace fan
