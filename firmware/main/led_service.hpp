#pragma once

// The LED receiver service: the protocol endpoint (frames from the host
// daemon), the baud hunt, host-liveness tracking, and boot/shutdown replay.
// start() spawns it as its own FreeRTOS task; call it once from app_main,
// after NVS flash is initialized and LittleFS is mounted.
namespace led
{
void start();

// whether the daemon is streaming: a live pixel frame within the host
// timeout. The host-is-up signal for a receiver with no power switch to
// watch the PSU with (ble.cpp) — the link can't say it on a UART, where
// link::hostPresent() is always true.
bool hostLive();
} // namespace led
