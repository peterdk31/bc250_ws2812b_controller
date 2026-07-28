#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"

// Serial link to the host daemon: the plain ESP32 reaches the host over UART0
// (wired to the USB bridge); native-USB chips (C3/C6/H2/S3) expose USB
// Serial/JTAG instead. The two use different drivers, selected in link.cpp by
// the IDF target and wrapped by this one interface.
//
// begin(baud): bring the link up. read(): pull up to maxlen bytes, blocking at
// most `wait` ticks (0 = don't block). setBaud(): change the line rate (UART
// only; a no-op over USB). flushInput(): drop buffered bytes after a rate
// change.
namespace link
{
void begin(uint32_t baud);
int read(uint8_t* buf, size_t maxlen, TickType_t wait);
void setBaud(uint32_t baud);
void flushInput();

// send bytes back to the host. The link is host→receiver for everything else;
// this exists only for the debug log backchannel (dbglog.cpp / protocol.hpp's
// LOG_SYNC frame). Best-effort and briefly bounded so a stalled or absent host
// (buffer full) can never block the caller.
void write(const uint8_t* buf, size_t len);

// whether a USB host is currently driving the bus (SOF keepalives seen).
// A UART can't observe its host, so that build always reports true and the
// host-restart re-arm (led_service.cpp) never fires there — a plain ESP32
// gets reset by the port-open/power-cycle instead.
bool hostPresent();
} // namespace link
