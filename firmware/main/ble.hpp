#pragma once

// BLE power remote and dashboard: the receiver (alive on 5VSB) advertises a
// small Bluetooth LE service so a phone can press the power button remotely
// and see what the box is doing — the Web Bluetooth page in docs/ is the
// intended client, but any BLE tool can drive it. Two halves:
//
// The *remote for the power switch* (power_switch.hpp): a command
// characteristic that stages the same gestures as the physical button —
// power on, the graceful ask-the-host shutdown, and the hold's hard cut (for
// a machine that crashed: the one moment a remote power button earns its
// keep) — and a status characteristic (read/notify) with the coarse PSU state.
//
// The *dashboard* (fan.hpp "the BLE dashboard's view"): what each fan header
// runs and why, the daemon's readings while it is up (CPU temperature and
// load, GPU load, every curve's input), and the fan config as the daemon runs
// it — which the phone can edit: the edit goes down the link as a message
// (hostreq.hpp), the daemon applies and persists it and pushes the new config
// back, and the phone sees it arrive. Everything from the daemon is JSON text
// this side never parses; it is the relay between the radio and the link, and
// the daemon stays the only place a curve is evaluated or stored.
//
// Radio policy: advertising runs in both PSU states — a crashed machine must
// be reachable, and it counts as "on" — but slow while the host is up
// (~1.3 s interval vs 300 ms while off), so the radio stays a rounding error
// next to the LED service's latch cadence. The strip itself is fed by SPI
// with DMA (render.cpp) precisely so radio interrupts can't tear its bits.
//
// Every command must carry a shared-secret token, chosen at flash time: the
// config lives in the small `blecfg` flash partition (the daemon config's
// "ble_remote" block, written by `make flash` / `make flash-ble`;
// tools/blecfg.py encodes). With the
// partition erased the feature is off and the BLE stack is never initialized
// — a board that hasn't opted in spends no RAM on this.
//
// Runs as its own task beside the NimBLE host task; its seams into the rest
// of the firmware are pwr::psuState() / pwr::remoteRequest() for the remote,
// and fan::snapshot() / dash::get() plus hostreq::post() for the dashboard. Removing the feature is deleting ble.*
// and unhooking those lines.
namespace ble
{
// bring the feature up (no-op unless blecfg enables it AND the power switch
// is active — there is nothing to remote-control without it). Called from
// app_main last: it is the slowest bring-up and the least critical.
void start();
} // namespace ble
