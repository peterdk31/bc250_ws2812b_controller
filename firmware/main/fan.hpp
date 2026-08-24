#pragma once

#include <cstdint>

// PWM fan controller: drives up to six 4-pin PC fans (25 kHz LEDC, the fan
// spec's frequency) from GPIOs chosen at flash time, standalone — like the
// power switch, it needs no daemon. The fans are powered externally (grounds
// common with this board); only their PWM inputs land here, so an unpowered
// fan just ignores the signal.
//
// Duty comes from three places, strongest first: a boot boost (every channel
// at boostDuty for boostSecs when the host's rail comes up — an AIO pump
// primes reliably at full speed), the last CMD_FAN_DUTY the daemon pushed
// (persisted in this module's own NVS namespace, so it applies on
// daemon-less boots too), and the flash-time defaults in the `fancfg`
// partition (`make flash FAN=on ...` / `make flash-fan`; erased = feature
// off).
//
// Runs as its own task and owns its own NVS namespace; it knows nothing of
// the LED service (the LED task hands it CMD_FAN_DUTY through setDuty and
// that is their whole acquaintance).
namespace fan
{
// bring the feature up and start its task. Called from app_main after
// pwr::start() — it reads pwr::senseState(), whose ADC is set up there — and
// before the slower filesystem mount, so the fans reach their duty early.
void start();

// a CMD_FAN_DUTY payload from the host: count(1) then count u8 percents,
// duty i for the i-th wired channel (see protocol.hpp). Called on the led_rx
// task; hands the values to the fan task under a critical section. A safe
// no-op while the feature is off.
void setDuty(const uint8_t* payload, uint16_t len);
} // namespace fan
