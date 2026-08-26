#pragma once

// BLE power remote: the receiver (alive on 5VSB) advertises a small
// Bluetooth LE service so a phone can press the power button remotely — the
// Web Bluetooth page in docs/ is the intended client, but any BLE tool can
// drive it. It is a *remote for the power switch* (power_switch.hpp) and
// nothing more: a command characteristic that stages the same gestures as
// the physical button — power on, the graceful ask-the-host shutdown, and
// the hold's hard cut (for a machine that crashed: the one moment a remote
// power button earns its keep) — and a status characteristic (read/notify)
// with the coarse PSU state.
//
// Radio policy: advertising runs in both PSU states — a crashed machine must
// be reachable, and it counts as "on" — but slow while the host is up
// (~1.3 s interval vs 300 ms while off), so the radio stays a rounding error
// next to the LED service's latch cadence. If the strip ever shows glitches
// with a phone connected, the RMT buffer in render.cpp (mem_block_symbols)
// is the knob to reach for before this policy is.
//
// Every command must carry a shared-secret token, chosen at flash time: the
// config lives in the small `blecfg` flash partition (`make flash BLE=on
// BLE_TOKEN=...` / `make flash-ble`, tools/blecfg.py encodes). With the
// partition erased the feature is off and the BLE stack is never initialized
// — a board that hasn't opted in spends no RAM on this.
//
// Runs as its own task beside the NimBLE host task; its one seam into the
// rest of the firmware is pwr::psuState() / pwr::remoteRequest(). Removing
// the feature is deleting ble.* and unhooking those two lines.
namespace ble
{
// bring the feature up (no-op unless blecfg enables it AND the power switch
// is active — there is nothing to remote-control without it). Called from
// app_main last: it is the slowest bring-up and the least critical.
void start();
} // namespace ble
