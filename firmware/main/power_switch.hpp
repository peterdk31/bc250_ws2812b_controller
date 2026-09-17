#pragma once

#include <cstdint>

// ATX power-switch service: with the receiver powered from the PSU's 5VSB
// standby rail, it stands in for the jumper the BC-250 otherwise needs on the
// FSP unit's PS_ON# line. Pressing a momentary button sinks PS_ON# to ground
// (via an N-channel MOSFET the pin gates — PSU on, board boots); holding it
// for HOLD_MS while the machine is up forces the PSU off; and a short press
// while the machine is up asks the host, over the link, to shut its OS down
// gracefully (hostreq.hpp — the receiver cannot do that itself, and does
// nothing to the PSU either way). An optional WAKE input — a 3.3 V
// active-high pulse from e.g. an OpenPuck when its Steam Controller connects —
// is a second way to power on and can never do anything else. An
// optional analog sense wire (BC-250 TPMS1 pin 9, the board's main 3.3 V rail
// — not pin 15, which is 3VSB) tells it when the board is
// actually up, which adds: follow a graceful OS shutdown down (release PS_ON#
// when the board turns itself off), and release if the board never comes up
// after a power-on (BOOT_TIMEOUT_MS). Both of those are suspended while the
// button is held — the failsafe for a mis-wired sense line, whose boot timeout
// would otherwise cut the power every 10 s and leave no window to reflash.
//
// Runs as its own task and owns its own NVS namespace; it knows nothing of
// the LED service. The wiring lives in the small `pwrcfg` flash partition,
// written at flash time (`make flash` / `make flash-pwr`); with the partition
// erased the feature is off. The four TUNINGS (hold, boot timeout, the two
// sense thresholds) start from the same partition but move at runtime: the
// daemon pushes the config's values (CMD_PWR_TUNING), the phone dials them
// (BLE control op 0x20), and they persist here in NVS layered over the
// partition's (cfgstore.hpp) — the pins never do, since re-pinning a line the
// machine's power hangs on is not a thing to do from a phone.
namespace pwr
{
// the tunings: everything about the switch that isn't a wire. Read by the
// task on every poll, so a change applies on the next one. The wire form
// (CMD_PWR_TUNING's payload, the control op's arguments, the NVS blob) is
// hold_ms(2) boot_timeout_ms(2) sense_low_mv(2) sense_high_mv(2),
// little-endian — proto::PWR_TUNING_LEN bytes.
struct Tuning
{
    uint16_t holdMs = 2000;         // hold the button this long to force off
    uint16_t bootTimeoutMs = 10000; // sense never came up after power-on -> release
    uint16_t senseLowMv = 800;      // hysteresis: below = board down...
    uint16_t senseHighMv = 2000;    // ...above = board up, between = hold state

    void encode(uint8_t* p) const;
    void decode(const uint8_t* p);
    // the ranges tools/pwrcfg.py enforces at flash time, so nothing on this
    // side can be talked into a switch that cuts the power at once (a hold
    // of 0) or never confirms a boot (inverted thresholds). *why names the
    // field when false.
    bool valid(const char** why = nullptr) const;
    bool operator==(const Tuning& o) const;
};

// a CMD_PWR_TUNING payload from the host, or the phone's op (ble.cpp): the
// eight wire bytes. Validated here and applied by the pwr task within one
// poll, then persisted. False — nothing applied — with the reason in *why
// when the feature is off, the payload is short, or a value is out of range.
// Callable from any task.
bool setTuning(const uint8_t* payload, uint16_t len, const char** why = nullptr);

// counts applied tuning changes (a poller notifies the phone on them)
uint32_t tuningSeq();

// ---- the BLE dashboard's view (ble.cpp) ----
// this board's side of the power switch — what it is wired to, what it is
// tuned to, what the sense wire reads right now; there even with the daemon
// down, which is when a phone most wants it
struct Snapshot
{
    bool active = false;     // the feature is on (psuState() >= 0)
    int8_t psOnPin = -1;     // the wiring, GPIO numbers, -1 = not wired
    int8_t buttonPin = -1;
    int8_t buttonGndPin = -1;
    int8_t sensePin = -1;    // -1 also when the pin isn't ADC-capable (no sense)
    int8_t ledPin = -1;
    int8_t wakePin = -1;
    uint8_t psu = 0;         // 0 off, 1 booting, 2 on
    uint16_t senseMv = 0xFFFF; // the last sense reading, 0xFFFF = none (no
                               // sense, or nothing sampled yet)
    Tuning tuning;           // in force
};
void snapshot(Snapshot& s);

// bring the feature up and start its task. Called from app_main right after
// nvs_flash_init and BEFORE the slower bring-up: if the chip rebooted (crash,
// watchdog, reflash) while it was holding PS_ON# low, that line is the
// board's power — re-assert with as small a gap as possible.
void start();

// what the board-power sense wire currently reads, for sibling features (the
// fan controller keys its pump boost off the host rail coming up):
//   -1  no sense available: feature off, sense unwired, or its ADC failed
//    0  rail down — including state OFF, where sense isn't sampled (PS_ON#
//       released means the rail is down by construction)
//    1  rail up (the debounced senseStable)
// Lock-free aligned reads of values only the pwr task writes, same
// justification as hostreq::acked(); callers poll, they don't get an edge.
int senseState();

// count of power-on events — PS_ON# asserts from a button press — since this
// chip booted. 0 until the first press. Deliberately NOT bumped by start()'s
// warm-reset re-hold (the machine was already up; the chip merely restarted
// under it), so a reader can tell "the machine was just switched on" from
// "this chip reset mid-session". The fan controller arms its pump boost on
// this. Lock-free aligned read, as senseState().
uint32_t powerOnSeq();

// does the power switch use this GPIO (button, PS_ON#, button ground, sense,
// LED, wake)? For sibling features choosing a pin at runtime — the fan controller
// refuses a PWM input on one of ours. False while the feature is off.
bool usesPin(int gpio);

// coarse PSU state for sibling features (the BLE remote's status
// characteristic): -1 = feature off, else 0 = OFF, 1 = BOOTING, 2 = ON.
// Lock-free aligned read, as senseState().
int psuState();

// stage a remote power request (ble.cpp): the same gestures as the physical
// button — REMOTE_ON is the press-while-off edge (ignored unless OFF),
// REMOTE_OFF the graceful short press that asks the host over the link
// (ignored unless ON, since only a running OS can answer), and
// REMOTE_OFF_HARD the hold: release PS_ON# and cut the PSU (ignored only in
// OFF; works in BOOTING too — a boot that never comes up is exactly a case
// for it). The hard cut exists remotely for the same reason the hold does —
// a wedged machine — just without walking to the box; the token gate in
// ble.cpp is what stands in for the finger. Callable from any task; the pwr
// task consumes it within one poll, so every power decision stays on that
// task. A no-op while the feature is off; a second request supersedes the
// first.
enum Remote : uint8_t
{
    REMOTE_NONE = 0,
    REMOTE_ON,
    REMOTE_OFF,
    REMOTE_OFF_HARD
};
void remoteRequest(Remote r);
} // namespace pwr
