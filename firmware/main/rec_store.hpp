#pragma once

#include <cstdint>

#include "recording.hpp"

// LittleFS mount point (mounted once by app_main; the recording files live
// under it, one per slot — see common/protocol.hpp)
#define LFS_BASE "/lfs"

// upper bound on one recording (frameCount*count*3) so a corrupt header can't
// trigger a huge allocation; far above any real animation (~tens of KB).
#define REC_MAX_BYTES (512u * 1024u)

// Device side of the recording store: LittleFS read/write around the shared
// header codec (common/recording.hpp). The format and field layout live there.
namespace rec_store
{
bool load(uint8_t slot, rec::Recording& out);

// write the slot's file (atomically — see the implementation); false if the
// filesystem refused it (not mounted, full, ...), in which case the slot keeps
// whatever it had. The caller logs the failure — a silent one looks exactly
// like an animation that was never configured.
bool save(uint8_t slot, const rec::Recording& r);

// the hash stored in a slot's file, or 0 if missing/invalid; lets an upload's
// BEGIN decide whether it's unchanged without reading the whole file. The
// hash counts only when the file's size matches what its header declares, so
// a truncated file (however it got that way) reads as "different" and the
// incoming upload rewrites it instead of being skipped forever
uint32_t storedHash(uint8_t slot);
} // namespace rec_store
