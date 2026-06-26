#pragma once

#include <stdint.h>

// the host→receiver wire format, shared by the daemon (host/serial_sink.hpp)
// and the ESP32 firmware so the two can never drift on the byte values.
//
// pixel frame:   SYNC0 SYNC1 pin lo hi anim_lo anim_hi xms_lo xms_hi <pixels...> checksum
// command frame: SYNC0 CMD_SYNC cmd len_lo len_hi <payload[len]> checksum
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

static const uint8_t SYNC0 = 0xAA; // both frame types start here
static const uint8_t SYNC1 = 0x55; // ...then this for a pixel frame
static const uint8_t CMD_SYNC = 0x56; // ...or this for a command frame

// bytes a pixel frame carries before the pixel data: SYNC0 SYNC1 pin count(2)
// anim(2) xms(2). Use this rather than a literal so a header change is one edit.
static const uint8_t PIX_HEADER = 9;

// The receiver no longer renders effects: the daemon records the configured
// power-on/shutdown effects to sequences of already-corrected pixel frames
// and streams them over; the receiver stores them in flash and replays them
// frame-by-frame during the windows the daemon can't drive the line (cold
// boot before the daemon is up, and after it exits on shutdown). These three
// commands carry one recording; CMD_SHUTDOWN triggers shutdown playback.

// play the stored shutdown recording. Optional payload: 2 bytes (little-endian)
// crossfade ms — the receiver dissolves from the last live frame into the
// recording over it, just like a live anim change. Absent/0 = snap.
static const uint8_t CMD_SHUTDOWN = 0x01;

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

// CMD_REC_FRAME: one recorded frame, payload = count*3 corrected RGB bytes
// (count from the preceding CMD_REC_BEGIN). Sent frameCount times in order.
static const uint8_t CMD_REC_FRAME = 0x03;

// CMD_REC_END: finish the recording for `slot` (payload: slot(1)). The
// receiver commits the buffered frames to flash (or, in skip mode, no-ops).
static const uint8_t CMD_REC_END = 0x04;

static const uint8_t SLOT_POWER_ON = 0x00;
static const uint8_t SLOT_SHUTDOWN = 0x01;

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
static const uint8_t REC_VERSION = 2; // 2 added loopStart; a v1 file is rejected

static const uint8_t REC_FLAG_LOOP = 0x01;

} // namespace proto
