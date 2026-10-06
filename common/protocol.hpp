#pragma once

#include <stdint.h>

// the host→receiver wire format, shared by the daemon (host/serial_sink.hpp)
// and the ESP32 firmware so the two can never drift on the byte values.
//
// pixel frame:   SYNC0 SYNC1    pin lo hi anim_lo anim_hi xms_lo xms_hi <R G B>*count checksum
// deep frame:    SYNC0 SYNC1_16 pin lo hi anim_lo anim_hi xms_lo xms_hi <RR GG BB>*count checksum
// command frame: SYNC0 CMD_SYNC cmd len_lo len_hi <payload[len]> checksum
//
// log frame:     SYNC0 LOG_SYNC seq(4) ms(4) len <text[len]> checksum
//   A receiver→host frame — a debug backchannel: when the daemon has
//   "serial.debug_log" set it periodically sends CMD_LOG_DRAIN with the
//   highest seq it has seen, and the receiver replies with one of these per
//   buffered log line newer than that (see firmware/main/dbglog.*). All
//   little-endian; checksum is the XOR of the seq, ms, len bytes and the text.
//   With the feature off the daemon never asks and the receiver never sends —
//   an older peer on either end simply never exchanges these.
//
// req frame:     SYNC0 REQ_SYNC req(1) nonce(4) checksum
//   A receiver→host frame the receiver sends unsolicited: something only the
//   host can do (see REQ_HOST_SHUTDOWN). The daemon answers with CMD_REQ_ACK
//   echoing req and nonce. Little-endian nonce, checksum is the XOR of req and
//   the four nonce bytes.
//
// msg frame:     SYNC0 MSG_SYNC kind(1) len_lo len_hi <payload[len]> checksum
//   The third receiver→host frame: a message *with a payload*, sent once, no
//   ack of its own (see MSG_EDIT / MSG_WATCH for what answers it).
//   Carries what the phone on the BLE dashboard asks of the daemon — at most
//   MSG_MAX bytes, a GATT write's own ceiling, so whatever the phone can
//   write fits one frame. Checksum is the XOR of kind, the two length bytes
//   and the payload bytes.
//
// the deep pixel frame is what the daemon sends: each channel is a
// little-endian 8.8 fixed-point value (the 8-bit strip code × 256, so
// 0..0xFF00) straight out of the host's correction LUT. The extra 8
// fractional bits are what the receiver's high-rate temporal dithering
// averages out on the strip (see common/dither.hpp) — without them, gamma +
// a low brightness collapse the levels and slow gradients step visibly.
// The plain 8-bit frame remains decodable (the shared parser widens it to
// 8.8) so an older daemon still drives a newer receiver.
//
// the pixel header carries, after pin and the 16-bit LED count, two more
// 16-bit fields: `anim`, an id for the animation producing this frame (the
// rendering effect instance — a composite like `cycle` reports its active
// child), and `xms`, the crossfade duration in ms. The receiver keeps the last
// frame it showed and, whenever `anim` changes from the previous frame,
// dissolves the new frame in over it across `xms` (common/fade.hpp). Both ride
// every frame so a frame is self-describing and a dropped frame can't strand a
// transition (the next frame still carries the changed id). xms == 0 snaps.
//
// the command frame uses a distinct second sync byte so the receiver's
// pixel-frame parser skips right over it. It's length-prefixed so it can
// carry a payload (a recording header, one recorded frame, ...), and ends in
// a checksum (XOR of cmd, the two length bytes and the payload) so noise that
// happens to start AA 56 almost never validates.
namespace proto
{

static const uint8_t SYNC0 = 0xAA;    // every frame type starts here
static const uint8_t SYNC1 = 0x55;    // ...then this for an 8-bit pixel frame
static const uint8_t CMD_SYNC = 0x56; // ...or this for a command frame
static const uint8_t SYNC1_16 = 0x57; // ...or this for an 8.8 deep pixel frame
static const uint8_t LOG_SYNC = 0x58; // ...or this for a receiver→host log frame
static const uint8_t REQ_SYNC = 0x59; // ...or this for a receiver→host request
static const uint8_t MSG_SYNC = 0x5A; // ...or this for a receiver→host message

// the longest msg frame payload (a GATT attribute's ceiling: nothing the
// phone writes can be longer)
static const uint16_t MSG_MAX = 512;

// the longest command payload either end handles. A pixel frame's own ceiling
// is the receiver's LED count, but the dashboard's views (CMD_VIEW) are
// sized by what the config holds, not by the strip — so the receiver's parser takes at least this much whatever its strip
// (common/receiver.hpp). The daemon never sends a longer one and says so.
static const uint16_t CMD_MAX = 2048;

// bytes a pixel frame (either depth) carries before the pixel data: SYNC0
// SYNC1/SYNC1_16 pin count(2) anim(2) xms(2). Use this rather than a literal
// so a header change is one edit.
static const uint8_t PIX_HEADER = 9;

// The receiver no longer renders effects: the daemon records the configured
// power-on/shutdown effects to sequences of already-corrected pixel frames
// and streams them over; the receiver stores them in flash and replays them
// frame-by-frame during the windows the daemon can't drive the line (cold
// boot before the daemon is up, and after it exits on shutdown). These three
// commands carry one recording; CMD_SHUTDOWN triggers shutdown playback.

// play the stored shutdown recording. Optional payload: 2 bytes (little-endian)
// crossfade ms — the receiver dissolves from the last live frame into the
// recording over it, just like a live anim change. Absent/0 = snap. An
// optional third byte carries flags: SHUTDOWN_POWERING_OFF when the daemon is
// exiting because the machine is going down (systemd is stopping the system),
// as opposed to a plain service stop or restart — the fan controller holds its
// live duties through a power-off (see CMD_FAN_LIVE) and lets them expire
// otherwise. Older firmware reads the two bytes it knows and ignores the rest.
static const uint8_t CMD_SHUTDOWN = 0x01;
static const uint8_t SHUTDOWN_POWERING_OFF = 0x01;

// CMD_REC_BEGIN: start streaming a recording for one slot. Payload (15 bytes,
// all little-endian) describes what follows and lets the receiver skip an
// unchanged upload without a return channel — it compares `hash` against the
// hash stored in the slot's file and, on a match, drops the incoming frames
// instead of rewriting flash (one transmit, zero wear). Layout:
//
//     slot(1) frameMs(2) count(2) pin(1) flags(1) frameCount(2) loopStart(2) hash(4)
//
//   slot       SLOT_POWER_ON / SLOT_SHUTDOWN
//   frameMs    delay between frames on replay (the effect's frame_ms)
//   count      LEDs per frame
//   pin        data pin the recording was rendered for
//   flags      bit0 = loop (replay wraps); else hold the last frame
//   frameCount number of CMD_REC_FRAME frames that follow
//   loopStart  frame to wrap back to when looping (lets a one-shot intro lead
//              into a looping tail — see the "sequence" config); 0 = whole thing
//   hash       FNV-1a over the recording's fields and pixel bytes
static const uint8_t CMD_REC_BEGIN = 0x02;

// CMD_REC_FRAME: one recorded frame, payload = count*6 bytes — per LED three
// little-endian 8.8 fixed-point channel values, the same depth the live deep
// pixel frame carries (count from the preceding CMD_REC_BEGIN). Sent
// frameCount times in order.
static const uint8_t CMD_REC_FRAME = 0x03;

// CMD_REC_END: finish the recording for `slot` (payload: slot(1)). The
// receiver commits the buffered frames to flash (or, in skip mode, no-ops).
static const uint8_t CMD_REC_END = 0x04;

static const uint8_t SLOT_POWER_ON = 0x00;
static const uint8_t SLOT_SHUTDOWN = 0x01;

// CMD_LOG_DRAIN: "send me every buffered log line newer than this". Payload is
// 4 bytes, the highest seq the host has already received (little-endian, 0 =
// everything the receiver still holds). The receiver answers with a log frame
// (LOG_SYNC, above) per matching line. Only sent when the daemon's debug
// backchannel is enabled; unknown to older firmware, which ignores it.
static const uint8_t CMD_LOG_DRAIN = 0x05;

// CMD_REQ_ACK: "heard you" for a req frame (REQ_SYNC, above). Payload is 5
// bytes, the req code and nonce echoed back, so an ack for a request the
// receiver has already abandoned can't retire the next one. The receiver
// repeats its request until this arrives; a daemon without the feature enabled
// never sends it, and the receiver reports the silence rather than assuming.
static const uint8_t CMD_REQ_ACK = 0x06;

// The fan controller (firmware/main/fan.cpp, README "Fans") drives up to
// FAN_CHANNELS PWM outputs, its "slots". The config's fans are a list, and
// each fan names its output: one of this board's headers (the board's header
// → GPIO map is flash-time, the fancfg partition), a raw receiver GPIO, or a
// PWM output on the HOST (a hwmon pwmN the daemon writes itself — no slot,
// nothing here). The fans whose output is on the receiver take the slots in
// the list's order, so slot i is "the i-th fan with a receiver output"; which
// pin a slot drives travels in its record (CMD_FAN_STANDALONE) and moves at
// runtime. Both fan commands are unknown to older firmware, which ignores
// them.
static const uint8_t FAN_CHANNELS = 6;
static const uint8_t FAN_NONE = 0xFF;

// CMD_FAN_STANDALONE: everything the receiver runs on its own — before the
// daemon is up, after it dies, on a daemon-less box — and which output each
// slot drives. Payload: FAN_CHANNELS records of FAN_HEADER_LEN bytes, slot 0
// first (FAN_STANDALONE_LEN in all; common/fanwire.hpp is the codec both
// ends use):
//
//     fallback(1) boost(1) boost_secs(1) ramp(1) kind(1) gpio(1) npts(1) pts[FAN_CURVE_POINTS][2]
//     out_kind(1) out(1)
//
// The push is the whole truth for every slot: a slot no fan uses is a record
// of 0xFF throughout (out_kind is then no output kind), and the receiver
// detaches it — its pin undriven, so a 4-pin fan left on it runs full, the
// fan spec's answer to a floating PWM wire. out_kind/out name the output:
// FAN_OUT_HEADER with out = the board's header number (1-based, resolved
// through the fancfg header map), or FAN_OUT_GPIO with out = a GPIO number;
// the receiver checks the pin (not another feature's, not a flash, strap or
// link pin, not another slot's) and leaves a slot it can't drive detached.
// fallback is the resting duty percent; boost the duty the slot runs for
// boost_secs after the host powers on (FAN_NONE = sits the boost out); ramp
// its slow-down rate in whole percent per second (0 = instant), which the
// receiver applies to its own curve. kind says what the fan's input is
// (FAN_KIND_*): a FALLBACK fan just runs its fallback; a GPIO fan is one
// whose curve the RECEIVER evaluates, reading a PWM signal's duty on GPIO
// `gpio` — the points are (input percent, duty percent) pairs, npts of them,
// sorted by x — and it keeps doing so with no daemon at all; a RECEIVER_TEMP
// fan is the same with the receiver chip's own temperature sensor as the
// input (whole °C points; gpio unused — a rough case-air reading, the die
// runs warmer than the air around it); a HOST fan's
// curve is the daemon's (a temperature, a load, a hwmon pwm) and the
// receiver runs the fallback until CMD_FAN_LIVE says otherwise. Every
// tuning is the fan's own — there is nothing global. The receiver persists
// all of it in NVS, so it applies on daemon-less boots too; the daemon sends
// it once at startup and again when an edit moves it. The same record, for
// one slot, is the BLE control op 0x10's argument.
static const uint8_t CMD_FAN_STANDALONE = 0x11;
static const uint8_t FAN_KIND_FALLBACK = 0;
static const uint8_t FAN_KIND_GPIO = 1;
static const uint8_t FAN_KIND_HOST = 2;
static const uint8_t FAN_KIND_RECEIVER_TEMP = 3; // firmware before it runs these as HOST
static const uint8_t FAN_OUT_HEADER = 1;
static const uint8_t FAN_OUT_GPIO = 2;
static const uint8_t FAN_CURVE_POINTS = 8; // = fancurve::MAX_POINTS
static const uint16_t FAN_HEADER_LEN = 9 + 2 * FAN_CURVE_POINTS;
static const uint16_t FAN_STANDALONE_LEN = FAN_CHANNELS * FAN_HEADER_LEN;
// what a fan runs when its config says nothing (the daemon's, the
// flasher's and the phone's defaults, so the three never disagree)
static const uint8_t FAN_DEFAULT_RAMP = 5;       // percent per second
static const uint8_t FAN_DEFAULT_BOOST_SECS = 5;

// CMD_FAN_LIVE: the duties the daemon's curves want right now. Payload:
// FAN_CHANNELS duty percents, by slot (FAN_NONE = not driven — a fallback or
// gpio or esp32_temp fan is always FAN_NONE here: the receiver runs
// those itself).
// Volatile: never persisted, and dropped back to the standalone fallback when
// the host goes silent (the LED service's host timeout), so a crashed daemon
// can't leave a fan pinned low under load. After a CMD_SHUTDOWN flagged
// SHUTDOWN_POWERING_OFF the receiver holds the last live duties instead — the
// machine is going down and its fans should wind down from where they are,
// not roar for the last few seconds. Sent on the daemon's 0.5 s tick when a
// value changes, plus a slow refresh so a receiver that reset mid-run picks
// the duties back up.
static const uint8_t CMD_FAN_LIVE = 0x09;

// 0x07 was CMD_FAN_DUTY, the flat percent array this replaced; retired, never
// reused (a receiver on that firmware ignores the two above and keeps its
// flash-time duties). 0x08 was CMD_FAN_STANDALONE with 23-byte records (no
// output): the record grew, and an old receiver took the longer payload's
// first bytes as its own layout — misaligned, and persisted. Retired, never
// reused: a receiver on that firmware ignores 0x11 and keeps what it has.
// The payload length is exact, so a future change of it can't be misread
// either way.

// The BLE dashboard (firmware/main/ble.cpp, docs/index.html, README "BLE
// remote"): the phone sees what the daemon runs and edits it live. The
// receiver is a relay for it — it holds the daemon's last word on each VIEW
// and serves it over GATT, and passes the phone's edits back; the daemon
// stays the only place any of it is evaluated or stored. A view is JSON text
// (UTF-8, no NUL) in the shape the daemon module that owns it documents; the
// receiver never parses one and keys them by id alone, so a new daemon view
// needs no new firmware. At most VIEW_MAX bytes each: a GATT value is 512
// bytes at most, so the phone reads a longer one in pages (ble.cpp, the page
// characteristic). The daemon never sends one over the ceiling and says so in
// the journal.
static const uint16_t VIEW_MAX = CMD_MAX - 1; // a CMD_VIEW's payload is the id + the view

// The views, by id (the receiver serves up to VIEWS of them; an id at or past
// that is dropped). The id is the page's handle on a view too, and is never
// reused for something else once shipped.
//   VIEW_FAN_CONFIG  the fan list as the daemon runs it (daemon/fans.hpp
//                    toJson): every fan's name, output, input, curve, boost,
//                    fallback and tunings, whether edits are accepted (the
//                    config file is writable), the list's revision (what an
//                    edit must name) and, once, why the last edit was
//                    refused. Sent at startup after CMD_FAN_STANDALONE and
//                    again whenever it changes — which is how an edit gets
//                    its answer, applied or refused.
//   VIEW_FAN_TELEM   what the curves read right now (fans.hpp telemetryJson):
//                    the top-level `sensors` temperature, CPU and GPU load,
//                    the BC-250 VRM controller's rails when one answers, and
//                    per fan its input, the duty the curve produced and, for
//                    a host output, who drives it. Only while a phone watches
//                    (MSG_WATCH), on the fan tick when a value changed plus a
//                    slow refresh.
//   VIEW_STRIP       the config's `strip` block as run (daemon/
//                    strip_remote.hpp): LED count, pin, reverse, brightness,
//                    gamma, white balance, plus the "scenes" — every rule
//                    whose condition is a bare `file:` path, whether its file
//                    exists right now and its color. Sent at startup, when a
//                    scene's file appears or disappears, and after an edit.
//   VIEW_SENSORS     the catalogue a fan could follow (fans.hpp sensorsJson):
//                    every labelled hwmon temperature by chip with its
//                    reading, and "_outs", every host pwm output with its
//                    duty and rpm. Only while a phone watches, every 5 s.
//   VIEW_POWER       the config's power_switch block as run (daemon/
//                    power_remote.hpp): the four tunings in the config's
//                    units, the wake pin, the short_press command and whether
//                    edits are accepted. Sent at startup, on a reload, after
//                    an edit.
static const uint8_t VIEW_FAN_CONFIG = 0;
static const uint8_t VIEW_FAN_TELEM = 1;
static const uint8_t VIEW_STRIP = 2;
static const uint8_t VIEW_SENSORS = 3;
static const uint8_t VIEW_POWER = 4;
static const uint8_t VIEWS = 8;

// CMD_VIEW: view(1) then the view's JSON text — the receiver keeps it as that
// view's value, replacing the last one (an empty text clears it: the daemon
// has nothing there, e.g. a config with no fans block, which the phone shows
// as it shows "no daemon"). Unknown to older firmware, which ignores it.
static const uint8_t CMD_VIEW = 0x12;

// 0x0A..0x0D and 0x0F were one command per view (CMD_FAN_CONFIG,
// CMD_FAN_TELEM, CMD_STRIP_CONFIG, CMD_FAN_SENSORS, CMD_PWR_CONFIG), each
// with its own buffer, GATT characteristic and msg kind on the receiver;
// CMD_VIEW replaced them all. Retired, never reused.

// The power switch (firmware/main/power_switch.cpp, README "Power switch")
// is wired at flash time (the `pwrcfg` partition: pins and the enable), but
// its four TUNINGS — how long a hold forces off, how long a boot may take
// before the PSU is released, and the sense wire's two thresholds — are
// values its task reads on every poll, so they move at runtime like the
// fans' standalone settings: the daemon pushes the config's power_switch
// block at startup, on a live reload and after a phone edit, and the receiver
// applies them at once and persists them in NVS, layered over pwrcfg's (which
// win again when re-flashed with different values — the fans' rule). One
// WIRE moves the same way: the wake input (pins.wake), an input that can
// only ever power the machine on, so re-pinning it from a phone risks
// nothing — unlike the other five, which stay flash-time.

// CMD_PWR_TUNING: hold_ms(2) boot_timeout_ms(2) sense_low_mv(2)
// sense_high_mv(2), little-endian, PWR_TUNING_LEN bytes — the same ranges
// tools/pwrcfg.py enforces (hold 100.., boot timeout 1000.., low < high); the
// receiver rejects anything else whole. Unknown to older firmware, which
// ignores it. The same eight bytes are the BLE control op 0x20's arguments,
// for a phone dialling a receiver with no daemon around.
static const uint8_t CMD_PWR_TUNING = 0x0E;
static const uint16_t PWR_TUNING_LEN = 8;

// CMD_PWR_WAKE: wake_pin(1) — the GPIO the wake input reads (README "Waking
// from an OpenPuck"), FAN_NONE (0xFF) = no wake input. The receiver checks
// the pin against the same facts a gpio:N fan source is checked against
// (fan::inputPinFree: not another feature's, not a flash, strap or link pin)
// and refuses one it can't read on, keeping the pin it has; applied within
// one poll and persisted like the tunings. Unknown to older firmware, which
// ignores it. The same byte is the BLE control op 0x21's argument.
static const uint8_t CMD_PWR_WAKE = 0x10;

// MSG_EDIT (msg frame): a phone's edit of a view — view(1) then the edit,
// JSON text in the shape the view's owner documents (a fan list operation
// naming the list's revision, a partial strip or power_switch object). The
// owner validates it exactly as it validates the config, applies it, writes
// it into its config block, and answers with the new view (CMD_VIEW). A
// refused fan edit is answered too — the list as it was, carrying why; a
// refused strip or power edit is not, and the phone's save times out.
static const uint8_t MSG_EDIT = 0x05;

// MSG_WATCH (msg frame): payload one byte, 1 = a phone is subscribed to the
// dashboard (repeated every ~10 s while it is), 0 = it left. The daemon sends
// its watched views (VIEW_FAN_TELEM, VIEW_SENSORS) only within ~30 s of a 1.
static const uint8_t MSG_WATCH = 0x02;

// 0x01, 0x03 and 0x04 were one msg kind per editable view (MSG_FAN_CONFIG,
// MSG_STRIP_CONFIG, MSG_PWR_CONFIG); MSG_EDIT replaced them. Retired, never
// reused.

// REQ_HOST_SHUTDOWN: "power yourself down, gracefully." The receiver's power
// switch sends this on a short button press while the machine is up — the
// ordinary PC power-button gesture, which nothing but the OS can honor. The
// daemon runs its configured poweroff command
// ("power_switch.short_press"); the receiver then just waits, and its
// existing sense-line follow-down releases PS_ON# when the board's rail
// collapses. So this asks the host to do something and never itself decides
// anything about the PSU — holding the button remains the only hard cut.
static const uint8_t REQ_HOST_SHUTDOWN = 0x01;

// reserved anim ids the receiver stamps on the recording frames it replays (it
// knows which slot is playing). This makes the boot→live and live→shutdown
// handoffs ordinary anim-id changes that crossfade through the same path as a
// live switch — so the firmware needs no separate "am I replaying?" state. The
// daemon's live ids come from a per-instance effect counter starting at 1 and
// must stay out of this top range (a collision would only cost one stray
// fade, since only adjacent frames are ever compared).
static const uint16_t ANIM_NONE = 0xFFFD;     // nothing shown yet / strip blanked
static const uint16_t ANIM_BOOT = 0xFFFE;     // power-on recording replay
static const uint16_t ANIM_SHUTDOWN = 0xFFFF; // shutdown recording replay

// on-flash recording header (little-endian), written once at the head of a
// slot's file, then frameCount * count*3 pixel bytes. MAGIC/VERSION let the
// receiver reject a stale or truncated file; `hash` is the same value
// CMD_REC_BEGIN carries, so the skip-unchanged check is a header read.
static const uint8_t REC_MAGIC0 = 'L';
static const uint8_t REC_MAGIC1 = 'R';
static const uint8_t REC_VERSION = 3; // 3: 8.8 deep pixels (2 added loopStart);
                                      // an older file is rejected and the slot
                                      // stays empty until the daemon re-uploads

static const uint8_t REC_FLAG_LOOP = 0x01;

} // namespace proto
