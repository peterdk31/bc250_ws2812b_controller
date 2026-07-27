#pragma once

// ATX power-switch service: with the receiver powered from the PSU's 5VSB
// standby rail, it stands in for the jumper the BC-250 otherwise needs on the
// FSP unit's PS_ON# line. A tap on a momentary button sinks PS_ON# to ground
// (via an N-channel MOSFET the pin gates — PSU on, board boots); holding it
// for HOLD_MS forces the PSU off. An
// optional analog sense wire (BC-250 TPMS1 pin 9) tells it when the board is
// actually up, which adds: follow a graceful OS shutdown down (release PS_ON#
// when the board turns itself off), and release if the board never comes up
// after a power-on (BOOT_TIMEOUT_MS).
//
// Runs as its own task and owns its own NVS namespace; it knows nothing of
// the LED service. The wiring and tuning live in the small `pwrcfg` flash
// partition, written at flash time (`make flash PWR=on ...` / `make
// flash-pwr`); with the partition erased the feature is off.
namespace pwr
{
// bring the feature up and start its task. Called from app_main right after
// nvs_flash_init and BEFORE the slower bring-up: if the chip rebooted (crash,
// watchdog, reflash) while it was holding PS_ON# low, that line is the
// board's power — re-assert with as small a gap as possible.
void start();
} // namespace pwr
