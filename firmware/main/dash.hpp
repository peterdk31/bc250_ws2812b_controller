#pragma once

#include <cstdint>

// The daemon's payloads for the BLE dashboard (ble.cpp), relayed verbatim.
//
// The phone's dashboard shows what the daemon knows — its fan config and
// readings, its strip settings — as JSON text the daemon writes for the phone
// (protocol.hpp CMD_FAN_CONFIG / CMD_FAN_TELEM / CMD_STRIP_CONFIG /
// CMD_FAN_SENSORS / CMD_PWR_CONFIG; the shapes are documented in
// daemon/fans.hpp, daemon/strip_remote.hpp and daemon/power_remote.hpp). This board
// never reads them: it keeps the last one of each kind here, for ble.cpp to
// serve as a GATT value and to notify when a new one lands. The setters run
// on the led_rx task as the frames arrive; the getters copy out under the
// lock from any task. None of it depends on the board's own fan or strip
// feature being on — a box with the fans wired elsewhere still gets the
// daemon's readings on its phone.
namespace dash
{
// each up to its protocol.hpp DASH_*_MAX — past a GATT attribute's 512 bytes
// for the larger ones, which the phone reads in pages (ble.cpp)
enum Slot
{
    FAN_CONFIG = 0,   // CMD_FAN_CONFIG: the fan list as the daemon runs it
    FAN_TELEM = 1,    // CMD_FAN_TELEM: what the curves read and run
    STRIP_CONFIG = 2, // CMD_STRIP_CONFIG: the strip's knobs and scenes
    FAN_SENSORS = 3,  // CMD_FAN_SENSORS: the catalogue for the input and output pickers
    PWR_CONFIG = 4,   // CMD_PWR_CONFIG: the power switch's tunings as the daemon runs them
    SLOTS = 5
};

static const uint16_t MAX_LEN = 2048; // the largest slot; a read buffer this big fits any

// a payload from the host. One this build can't hold is a newer daemon's
// and is dropped rather than served truncated for the phone to misread.
void set(Slot slot, const uint8_t* payload, uint16_t len);

// copy the last payload into out (max bytes); returns its length, 0 when
// none has arrived (or out is too small). *seq counts arrivals, so a poller
// can tell a new one from the same one; *ageMs is how long ago the last
// arrived (0xFFFFFFFF = never). Both optional.
uint16_t get(Slot slot, uint8_t* out, uint16_t max, uint32_t* seq = nullptr,
             uint32_t* ageMs = nullptr);
} // namespace dash
