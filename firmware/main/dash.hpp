#pragma once

#include <cstdint>

#include "protocol.hpp"

// The daemon's views for the BLE dashboard (ble.cpp), relayed verbatim.
//
// The phone's dashboard shows what the daemon knows — its fan config and
// readings, its strip settings, its power switch settings — as JSON text the
// daemon writes for the phone, one VIEW each (protocol.hpp CMD_VIEW; the
// shapes are the daemon modules' own). This board never reads them and
// knows none of them by name: it keeps the last one of each id here, for
// ble.cpp to serve and to notify when a new one lands, so a new daemon view
// needs nothing from this firmware. The setters run on the led_rx task as
// the frames arrive; the getters copy out under the lock from any task. None
// of it depends on the board's own fan or strip feature being on — a box
// with the fans wired elsewhere still gets the daemon's readings on its phone.
//
// Memory: a view's buffer is taken from the heap when its first payload
// arrives and only ever grows (to the largest that view has sent, at most
// VIEW_MAX), so a board without a daemon holds none, and one with a daemon
// holds what that daemon's views need.
namespace dash
{
static const uint8_t VIEWS = proto::VIEWS;
static const uint16_t MAX_LEN = proto::VIEW_MAX; // a read buffer this big fits any view

// a view from the host. An id this build can't hold, a payload over
// MAX_LEN, or one the heap can't make room for is dropped rather than served
// truncated for the phone to misread (the last one stays). An empty payload
// clears the view.
void set(uint8_t view, const uint8_t* payload, uint16_t len);

// copy the last payload into out, its first max bytes if it is longer;
// returns the bytes copied, 0 when none has arrived. *len is the whole
// payload's length; *seq counts arrivals, so a poller can tell a new one from
// the same one; *ageMs is how long ago the last arrived (0xFFFFFFFF = never).
// All optional, and copied together with the bytes.
uint16_t get(uint8_t view, uint8_t* out, uint16_t max, uint16_t* len = nullptr,
             uint32_t* seq = nullptr, uint32_t* ageMs = nullptr);
} // namespace dash
