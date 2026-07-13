#pragma once

// The LED receiver service: the protocol endpoint (frames from the host
// daemon), the baud hunt, host-liveness tracking, and boot/shutdown replay.
// start() spawns it as its own FreeRTOS task; call it once from app_main,
// after NVS flash is initialized and LittleFS is mounted.
namespace led
{
void start();
} // namespace led
