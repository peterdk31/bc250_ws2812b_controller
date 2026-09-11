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
// msg frame:     SYNC0 MSG_SYNC kind(1) len(1) <payload[len]> checksum
//   The third receiver→host frame: a message *with a payload*, sent once, no
//   ack of its own (see MSG_FAN_CONFIG / MSG_FAN_WATCH for what answers it).
//   Carries what the phone on the BLE dashboard asks of the daemon. Checksum
//   is the XOR of kind, len and the payload bytes.
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
// FAN_CHANNELS PWM outputs, one per header. Both fan commands carry one byte
// per header, header1 first, and FAN_NONE (0xFF) in a duty slot means "this
// header is not the daemon's to drive" — the receiver leaves it exactly as it
// is (a disabled header in the config). The pins themselves live in the
// receiver's `fancfg` flash partition, written at flash time from the same
// config block; these commands only ever adjust speeds. Both are unknown to
// older firmware, which ignores them.
static const uint8_t FAN_CHANNELS = 6;
static const uint8_t FAN_NONE = 0xFF;

// CMD_FAN_STANDALONE: what each header runs when the daemon is NOT driving it
// — before the daemon is up, after it dies, on a daemon-less box. Payload
// (1 + 2*FAN_CHANNELS bytes): boost_secs(1), then per header fallback(1)
// boost(1). fallback is the resting duty percent (FAN_NONE = leave the
// header alone); boost is the duty percent the header runs for boost_secs
// after the host powers on (FAN_NONE = this header sits the boost out). The
// receiver persists these in NVS, so they apply on daemon-less boots too,
// and the daemon sends them once at startup — there is nothing to re-send.
static const uint8_t CMD_FAN_STANDALONE = 0x08;

// CMD_FAN_LIVE: the duties the daemon's curves want right now. Payload:
// FAN_CHANNELS duty percents (FAN_NONE = not driven). Volatile: never
// persisted, and dropped back to the standalone fallback when the host goes
// silent (the LED service's host timeout), so a crashed daemon can't leave a
// fan pinned low under load. After a CMD_SHUTDOWN flagged SHUTDOWN_POWERING_OFF
// the receiver holds the last live duties instead — the machine is going down
// and its fans should wind down from where they are, not roar for the last
// few seconds. Sent on the
// daemon's 0.5 s tick when a value changes, plus a slow refresh so a receiver
// that reset mid-run picks the duties back up.
static const uint8_t CMD_FAN_LIVE = 0x09;

// 0x07 was CMD_FAN_DUTY, the flat percent array this replaced; retired, never
// reused (a receiver on that firmware ignores the two above and keeps its
// flash-time duties).

// The BLE dashboard (firmware/main/ble.cpp, docs/index.html, README "BLE
// remote"): the phone sees what the fans are doing and edits the curves live.
// The receiver is a relay — it holds the daemon's last word on both and serves
// it over GATT; the daemon stays the only place curves are evaluated or stored.
// All three payloads below are JSON text (UTF-8, no NUL), in the shape
// daemon/fans.hpp documents; the receiver never parses them.

// CMD_FAN_CONFIG: the fan config as the daemon runs it — per enabled header
// its name, source kind, curve, boost and fallback, plus hysteresis, ramp,
// boost_seconds and whether edits are accepted (the config file is writable).
// At most 512 bytes (a GATT
// attribute's ceiling; the daemon refuses to send a larger one and says so).
// Sent once at startup after CMD_FAN_STANDALONE and again whenever it changes
// (a phone edit was applied), which is also how a MSG_FAN_CONFIG gets its
// answer: the receiver notifies the phone with the new truth. Unknown to older
// firmware, which ignores it.
static const uint8_t CMD_FAN_CONFIG = 0x0A;

// CMD_FAN_TELEM: what the curves are reading right now — the top-level
// `sensors` temperature, CPU and GPU load, and per header its input and the
// duty the curve produced. At most 384 bytes. Only sent while a phone is
// watching (MSG_FAN_WATCH keeps that alive), on the fan tick when a value
// changed plus a slow refresh — nothing is read or sent for a dashboard
// nobody has open.
static const uint8_t CMD_FAN_TELEM = 0x0B;

// CMD_STRIP_CONFIG: the strip's dashboard view — the config's `strip` block
// as the daemon runs it (LED count, pin, reverse, brightness, gamma, white
// balance) plus the "scenes": every rule whose condition is a bare `file:`
// path, with whether its file exists right now and, when its settings carry
// one, its color. JSON text in the shape daemon/strip_remote.hpp documents, at
// most 512 bytes. Sent once at startup, whenever a scene's file appears or
// disappears, and after every applied edit (the answer to a MSG_STRIP_CONFIG).
// Unknown to older firmware, which ignores it.
static const uint8_t CMD_STRIP_CONFIG = 0x0C;

// MSG_FAN_CONFIG (msg frame): a phone's edit — a partial object of the same
// shape holding only what changed (one header's curve/boost/fallback/name/
// enabled, or the three globals). The daemon validates it exactly as it
// validates the config, applies it live, writes it into its config file's
// fans block (README "Fans"), and answers with CMD_FAN_CONFIG. An invalid or
// read-only edit is dropped with a journal line; the phone's timeout on the
// answering CMD_FAN_CONFIG is what tells its user.
static const uint8_t MSG_FAN_CONFIG = 0x01;

// MSG_FAN_WATCH (msg frame): payload one byte, 1 = a phone is subscribed to
// the dashboard (repeated every ~10 s while it is), 0 = it left. The daemon
// sends CMD_FAN_TELEM only within ~30 s of a 1.
static const uint8_t MSG_FAN_WATCH = 0x02;

// MSG_STRIP_CONFIG (msg frame): a phone's edit of the strip view — a partial
// object: any of brightness / gamma / white_balance / reverse (applied to the
// live strip at once and written into the config's strip block), and/or
// "scenes": [{ "p": path, "on": bool }] to create or remove a scene's file
// (nothing written to the config — the rule reacts on its next tick) or
// [{ "p": path, "color": "rrggbb" }] to change a scene rule's color setting
// (written into that rule). Answered with CMD_STRIP_CONFIG, refused like a
// fan edit.
static const uint8_t MSG_STRIP_CONFIG = 0x03;

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
