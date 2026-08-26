#pragma once

#include <cstdint>

#include "recording.hpp"

// LittleFS mount point (mounted once by app_main; the recording files live
// under it, one per slot — see common/protocol.hpp)
#define LFS_BASE "/lfs"

// upper bound on one recording so a corrupt header can't declare an absurd
// stream; far above any real animation (~100 KB). Bounds flash, not RAM:
// recordings are streamed to and from the filesystem frame by frame and are
// never held whole in memory. Two slots plus their in-flight temp files must
// fit the 2 MB storage partition, so keep this ≤ 512 KB.
#define REC_MAX_BYTES (512u * 1024u)

// Device side of the recording store: LittleFS streaming around the shared
// header codec (common/recording.hpp). The format and field layout live
// there. Nothing here buffers pixels — an upload is appended to a temp file
// as its frames arrive, and replay reads one frame per tick — so a recording
// of any allowed size costs the same few hundred bytes of RAM (the RAM
// buffering this replaced was a ~100 KB contiguous allocation that could
// abort the whole chip; exceptions are off, so a failed new is a panic).
namespace rec_store
{
// --- streamed upload (CMD_REC_BEGIN / FRAME / END) ---
// saveBegin opens the slot's temp file and writes the header (with `hash`
// straight off the wire — the pixels never exist here to recompute it from;
// the caller verifies the stream against it, see rec::RecordingReceiver);
// saveFrame appends one vetted frame; saveCommit closes and atomically
// renames over the slot, so an interrupted stream can never leave a
// valid-looking header over truncated data — the slot keeps its previous
// recording. A write failure latches: later frames are dropped and commit
// returns false. saveAbort discards the temp file.
bool saveBegin(uint8_t slot, const rec::Recording& meta, uint32_t hash);
void saveFrame(const uint8_t* px, size_t len);
bool saveCommit();
void saveAbort();

// --- streamed replay ---
// playOpen validates the slot's file (header, count ≤ maxCount, and its size
// exactly matching what the header declares) and fills `meta` — geometry and
// timing only, data stays empty; playRead fetches one frame into the
// caller's buffer. One replay stream at a time (only one recording ever
// plays); playSlot says which slot it's on (-1 none) so an upload commit can
// close a replay of the same file before renaming over it — LittleFS can't
// rename over an open file.
bool playOpen(uint8_t slot, rec::Recording& meta, uint16_t maxCount);
bool playRead(uint16_t frame, uint8_t* out, size_t len);
void playClose();
int playSlot();

// the hash stored in a slot's file, or 0 if missing/invalid; lets an upload's
// BEGIN decide whether it's unchanged without reading the whole file. The
// hash counts only when the file's size matches what its header declares, so
// a truncated file (however it got that way) reads as "different" and the
// incoming upload rewrites it instead of being skipped forever
uint32_t storedHash(uint8_t slot);
} // namespace rec_store
