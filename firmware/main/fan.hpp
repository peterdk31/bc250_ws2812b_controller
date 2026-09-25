#pragma once

#include <cstdint>

#include "protocol.hpp"

// PWM fan controller: drives up to six 4-pin PC fans (25 kHz LEDC, the fan
// spec's frequency) — its "slots", one LEDC channel each — standalone, like
// the power switch: it needs no daemon. The fans are powered externally
// (grounds common with this board); only their PWM inputs land here, so an
// unpowered fan just ignores the signal.
//
// Which pin each slot drives is a runtime setting, like its duty: a slot's
// record names one of the board's headers (the header → GPIO map is the one
// flash-time part, the `fancfg` partition — a fact about the board) or a raw
// GPIO, checked here against everything else the pin could be (setHeader,
// setupOutputs). A slot no fan uses drives nothing: its pin is left
// undriven, so a 4-pin fan on it runs full.
//
// Each slot's duty comes from four places, strongest first:
//
//  - the boot boost: the slot's own boost duty for its own boost length
//    after the host powers on (an AIO pump primes reliably at full speed; a
//    slot with no boost sits it out). Armed by the power switch's power-on event and fired
//    when its sense wire confirms the rail, so a warm reset of this chip never
//    re-fires it — see boostCheck.
//  - this board's own curve, for a fan whose input is "gpio:N": the duty of a
//    PWM signal on GPIO N (the BC-250's own fan header, wired over) is
//    sampled here and run through the slot's curve — so it follows the
//    board's BIOS fan curve with no daemon and the machine off.
//  - the live duty: what the daemon's fan curves want right now
//    (CMD_FAN_LIVE), for the fans whose input only the host can read.
//    Volatile — never persisted — and it expires when the daemon stops
//    refreshing it, so a dead daemon can't leave a fan pinned low under
//    load. After the host announces that the machine is powering off it is
//    held instead: the fans wind down from where they are.
//  - the standalone settings, one record per slot (common/fanwire.hpp): the
//    output, the resting ("fallback") duty, the boost and its length, the
//    ramp, and the input kind with a gpio slot's pin and curve. Flash-time
//    defaults come from the `fancfg` partition (`make flash` / `make
//    flash-fan` write it from the daemon config's "fans" block); the daemon
//    pushes the same values once at startup (CMD_FAN_STANDALONE) and they
//    persist in this module's own NVS namespace, so an edited config applies
//    on daemon-less boots too. The phone can rewrite a slot's whole record
//    (setHeader) — add a fan, move it to another header, dial its resting
//    duty, point it at a fallback or gpio input with a curve, remove it —
//    which persists the same way: a box with no daemon is still a fan
//    controller.
//
// Runs as its own task and owns its own NVS namespace; it knows nothing of
// the LED service. The LED task hands it the two fan commands and one word —
// "the host is shutting down" — and that is their whole acquaintance.
namespace fan
{
// read the `fancfg` partition and the persisted settings, and plan the pins
// the slots will drive, without bringing the feature up. Called from
// app_main BEFORE pwr::start() (and after nvs_flash_init()): the power switch
// checks a saved wake pin against inputPinFree() there, and that answer has
// to know where the fan outputs are before the pwr task's first poll (which
// runs the moment its task exists, ahead of anything app_main does next).
// start() runs on what this read; calling it again is a no-op.
void readConfig();

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

// the phone's edit (ble.cpp): one slot's whole standalone record, in
// CMD_FAN_STANDALONE's per-slot layout (FAN_HEADER_LEN bytes, protocol.hpp)
// — the same setting the host pushes, changed one slot at a time and
// persisted the same way; the next daemon start pushes the config file's
// values again, so the file stays the source of truth on a running machine.
// A record with no output (all 0xFF) removes the slot's fan. Otherwise its
// output is a header of this board or a GPIO, and its input one this board
// can run — FAN_KIND_FALLBACK (the fan just runs its fallback),
// FAN_KIND_GPIO with the input pin and a curve of npts (input %, duty %)
// pairs sorted by input, or FAN_KIND_HOST (the fallback until a daemon
// drives it). Called on the NimBLE host task. False, with the reason in
// *why, when a value is out of range, the curve is malformed, the output is
// no pin this board can drive a PWM on, or the input none it can read one on
// (another feature's, a flash or strap pin, the USB pair, another slot's
// output or input): nothing is applied then.
bool setHeader(uint8_t slot, const uint8_t* rec, uint16_t len, const char** why);

// can this board read an input on GPIO g — a gpio:N input's PWM, or the
// power switch's wake pulse? The check behind setHeader's pin refusal and
// inputPins(): not another feature's pin (the power switch's, via
// pwr::usesPin; the strip's), not a fan header's wire, not one of this
// feature's PWM outputs, not a flash pad, the host link or a boot strap. A
// slot's own INPUT in use is not excluded (two fans may read one PWM) —
// readsPin() says that. *why names the reason when false.
bool inputPinFree(int g, const char** why);

// is GPIO g a PWM input some slot reads right now?
bool readsPin(int g);

// the GPIOs a gpio:N input (or the wake input) may read on this board right
// now, bit N set: exactly the pins inputPinFree() accepts. The phone lists
// these instead of asking for a number. Known once readConfig() has run,
// whether or not the fan feature is on; it changes when the power switch's
// wake pin moves and when a fan's output does, and the page re-reads it then.
uint64_t inputPins();

// the GPIOs a new gpio:N OUTPUT could drive right now, bit N set — the
// board's header pins left out (the phone offers those as headers), and so
// is every pin a slot already drives or reads. The phone's output picker.
uint64_t outputPins();

// the board's header map: out[n-1] = the GPIO of header n, FAN_NONE where
// the board has no such header (fancfg; all NONE with no partition)
void headerPins(uint8_t* out);

// the host sent CMD_SHUTDOWN flagged as a power-off: hold the live duties
// through the power-down instead of letting them expire to the fallback.
void hostShutdown();

// ---- the BLE dashboard's view (ble.cpp) ----
//
// The daemon's own payloads for the dashboard (its fan config and readings)
// are kept by dash.cpp; this is the board's side of the picture — what the
// PWM outputs are doing right now, per header, which is there even with the
// daemon down
struct Snapshot
{
    bool active = false;   // the PWM feature is on
    bool boosting = false; // some header's boot boost window is running
    bool hold = false;     // the host is powering off, live duties held
    bool live = false;     // the daemon's live duties are in force
    uint8_t wired[proto::FAN_CHANNELS];  // the slot drives a pin right now
    uint8_t duty[proto::FAN_CHANNELS];   // the duty actually applied
    uint8_t source[proto::FAN_CHANNELS]; // SRC_*: where that duty came from
    uint8_t fallback[proto::FAN_CHANNELS]; // the resting duty in force (what
                                           // the phone's slider edits), NONE undriven
    uint8_t kind[proto::FAN_CHANNELS];   // FAN_KIND_* in force, NONE undriven
    uint8_t in[proto::FAN_CHANNELS];     // a gpio slot's sampled input %,
                                         // NONE when there is no reading
};
static const uint8_t SRC_NONE = 0;     // slot drives no pin
static const uint8_t SRC_FALLBACK = 1;
static const uint8_t SRC_LIVE = 2;
static const uint8_t SRC_BOOST = 3;
static const uint8_t SRC_CURVE = 4;    // this board's own gpio curve
void snapshot(Snapshot& s);

// the standalone settings in force, in CMD_FAN_STANDALONE's layout
// (FAN_STANDALONE_LEN bytes) — what the phone reads to show and edit the
// receiver's fans with no daemon around. Returns the length written, 0 while
// the feature is off. *seq counts changes, so a poller can notify on them.
uint16_t standalone(uint8_t* out, uint16_t max, uint32_t* seq = nullptr);
} // namespace fan
