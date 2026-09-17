#pragma once

#include <cstddef>
#include <cstdint>

#include "nvs.h"

// The two storage patterns every standalone feature on this board shares —
// the power switch, the fan controller and the BLE remote each used to carry
// their own copy:
//
//  - a flash-time config partition (`pwrcfg`, `fancfg`, `blecfg`): a 4 KB
//    data partition written by esptool from the daemon config's block, read
//    once at boot. partition() is the read; the caller checks the magic and
//    decodes, since each has its own layout and versioning.
//
//  - a runtime override of some of those flash-time values, layered over
//    them: what the daemon pushes at startup and the phone dials (the fans'
//    standalone settings, the power switch's tunings). It persists in NVS so a
//    daemon-less boot still runs it — saved BESIDE the flash-time defaults it
//    was saved under, and applied only while those are still the defaults on
//    the chip. A partition re-flashed with different values is the user
//    re-deciding at the keyboard and outranks a stale push or dial, the same
//    newer-default-wins rule the LED service applies to its saved baud.
namespace cfgstore
{
// read the first `len` bytes of the named data partition into out. False
// when the partition is missing from the table (an older layout — *found says
// which) or the read fails.
bool partition(const char* label, uint8_t* out, size_t len, bool* found = nullptr);

// the saved override under `key`, if it was saved under exactly the `len`
// bytes of `base` (kept under `baseKey`). False = nothing usable saved: run
// the defaults.
bool load(nvs_handle_t nvs, const char* key, const char* baseKey, const uint8_t* base,
          size_t len, uint8_t* out);

// save `value` as the override, remembering `base` as the defaults it
// overrides. Commits.
void save(nvs_handle_t nvs, const char* key, const char* baseKey, const uint8_t* value,
          const uint8_t* base, size_t len);
} // namespace cfgstore
