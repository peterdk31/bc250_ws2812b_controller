#pragma once

#include <stdint.h>

// the host→receiver wire format, shared by the daemon (ws2812_serial.hpp)
// and the ESP32 firmware so the two can never drift on the byte values.
//
// pixel frame:   SYNC0 SYNC1 pin lo hi <pixels...> checksum
// command frame: SYNC0 CMD_SYNC cmd len_lo len_hi <payload[len]> checksum
//
// the command frame uses a distinct second sync byte so the receiver's
// pixel-frame parser skips right over it. It's length-prefixed so it can
// carry a payload (the receiver config), and ends in a checksum (XOR of
// cmd, the two length bytes and the payload) so noise that happens to
// start AA 56 almost never validates.
namespace proto
{

static const uint8_t SYNC0 = 0xAA; // both frame types start here
static const uint8_t SYNC1 = 0x55; // ...then this for a pixel frame
static const uint8_t CMD_SYNC = 0x56; // ...or this for a command frame

static const uint8_t CMD_SHUTDOWN = 0x01; // play the shutdown effect (no payload)
static const uint8_t CMD_CONFIG = 0x02;   // set power-on/shutdown effects

// CMD_CONFIG payload: two NUL-terminated slot strings, power-on then
// shutdown. A slot string is the effect name on the first line, then one
// `key=value` setting per line:
//
//     rainbow\ncycles_per_second=1\n\0shutdown\ncolor=0028ff\n\0
//
// an empty slot string means "use the firmware's built-in default".

} // namespace proto
