#pragma once

#include <stdint.h>

// The temporal dither the receiver applies when it rounds the wire's 8.8
// fixed-point channel values (see common/protocol.hpp) down to the strip's
// 8 bits. The receiver re-latches the strip every couple of milliseconds —
// far faster than frames arrive — and biases each latch's round-down with an
// ordered threshold, so a sub-code value is shown as a duty cycle between two
// adjacent codes. At several hundred Hz the toggling sits way above flicker
// fusion, so the eye sees only the average: dim gradients glide instead of
// stepping, with none of the sparkle a frame-rate dither has.
//
// Host code doesn't dither at all any more — the daemon ships the full 8.8
// values and the rounding decision moves to the one place that can make it
// fast enough to be invisible. The on-screen viewer shows the plain rounded
// value instead: on a monitor that IS the average the strip's dither produces.
//
// Two exceptions, applied by the receiver at the latch (see refreshStrip):
//
//  - values below code 1 are rounded to nearest, never dithered —
//    duty-cycling an unlit LED means 100%-contrast flashes from black,
//    which the eye catches (through a pale diffuser, glaringly) at rates
//    that pass unseen between two lit codes.
//
//  - a duty cycle's pulse rate is fraction × latch rate, so a near-code
//    fraction pulses slowly — value 2.02 would show code 3 a few times a
//    second, a visible twinkle at dim codes where one step is a big
//    relative jump. Fractions whose pulse (or gap) rate would fall below
//    the receiver's blink floor round to the nearest code instead: a
//    steady, slightly-off level in place of a slow blink.
namespace dither
{

// reverse the bits of a byte (0b00000001 -> 0b10000000)
inline uint8_t bitReverse8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// ordered threshold in [0,255] for one strip refresh (`tick`) and LED
// (`pixel`): rounding v up when (v & 0xFF) + threshold carries realizes v's
// fraction as an exact duty cycle over any 256 consecutive ticks.
// Bit-reversing the tick (a van der Corput sequence: 0, 128, 64, 192, ...)
// makes consecutive refreshes land at opposite ends of the range, so a
// fraction is approximated within a handful of latches rather than over a
// slow ramp. The per-pixel offset keeps neighbours from toggling in lockstep;
// one threshold is shared across a pixel's three channels so a dim colour
// rounds up together and stays on-hue instead of fraying into
// single-channel dots.
inline uint8_t threshold(uint32_t tick, int pixel)
{
    return (uint8_t)(bitReverse8((uint8_t)tick) + (uint8_t)(pixel * 59));
}

} // namespace dither
