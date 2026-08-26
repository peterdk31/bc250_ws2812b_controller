#pragma once

#include <cstdint>

// upper bound on strip geometry; sizes the static 8.8 frame buffers in
// render.cpp and bounds every count that arrives from the wire or a
// recording. Deliberately snug: every buffer it sizes is 6 bytes per LED and
// there are several (three in render.cpp, two in the shared parser), so 2048
// here cost ~60 KB of a C3's RAM for a 33-LED strip. Driving a longer strip
// than this means raising it and reflashing.
#define MAX_LEDS 100

// The display path: owns the led_strip device and the 8.8 fixed-point frame
// buffers, and is the only code that touches pixels.
//
// Crossfading lives here (mirrored by the host's viewer) rather than in the
// daemon: we keep the last frame shown and, when the incoming animation's id
// changes, dissolve the new one in over it with the shared fader. Every frame
// — live or replayed (boot/shutdown recordings) — goes through show() carrying
// an id, so the boot->live and live->shutdown handoffs are just ordinary id
// changes; there's no "am I replaying?" state to track. Live ids ride the
// wire; replay frames get a reserved per-slot id (proto::ANIM_*), which the
// caller knows from the slot it's playing (see common/fade.hpp).
//
// All frames are 8.8 fixed point (count*3 uint16 channel values, see
// common/protocol.hpp). show() only records the newest frame; tick()
// re-latches the strip every DITHER_REFRESH_US, blending an active dissolve
// and dithering the fraction away per latch — so both the dither *and* a
// crossfade run at strip-refresh rate, not at the (much slower) frame rate.
namespace render
{
// (re)create the strip device on this geometry. On failure the device stays
// down (up() reports false) and every call below is a safe no-op, so a bad
// pin/count can't brick the receiver — it just stays dark until a good frame.
void init(uint16_t count, uint8_t pin);

bool up();        // a strip device exists
uint16_t count(); // geometry of the current device
uint8_t pin();

// zero every pixel and latch it out; the next frame (a returning host, or a
// replay) dissolves up from black rather than snapping
void blank();

// the one place a frame reaches the display path. Every source — live frames
// and replayed recordings alike — calls this with the animation's id; when the
// id differs from what's on screen we start a dissolve from the last blended
// frame (as long as the geometry matches), then let the latch render it. `px`
// is count*3 8.8 channel values. Latches immediately, so a fresh frame never
// waits on the timer.
void show(uint16_t animId, uint16_t count, uint16_t xms, const uint16_t* px);

// between frames, keep re-latching the newest (blended) frame with a fresh
// dither threshold, so sub-code values render as high-rate duty cycles — and
// an active dissolve advances at this rate too, not at frame rate. Call every
// loop pass; latches only once DITHER_REFRESH_US has elapsed.
void tick();

// loan of the per-latch blend buffer (MAX_LEDS*3 8.8 values) so a replay
// frame can be decoded in place before show()ing it, instead of a second
// 12 KB buffer. Contents are only valid until the next show()/tick().
uint16_t* decodeBuf();

// hardware proof: cycle the strip red -> green -> blue forever on the given
// pin, with nothing else running (see LED_SELFTEST in led_service.cpp)
[[noreturn]] void selftest(uint16_t count, uint8_t pin);
} // namespace render
