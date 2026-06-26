#pragma once

#include <stdint.h>
#include <vector>

// A consumer of finished wire frames. The daemon builds a list of these
// from config and feeds every rendered frame to each one: real hardware
// (SerialSink), the on-screen preview (VirtualSink), and so on. The list is
// the fan-out — there's no combiner type. An empty list is valid and simply
// renders to nothing (e.g. a headless run with no viewer attached).
//
// The bytes handed in are exactly the bytes on the wire (header + pixels +
// checksum, see common/protocol.hpp), already brightness/gamma/white-balance
// corrected by the Strip, so every sink sees precisely what the strip shows.
struct Sink
{
    virtual ~Sink() = default;

    // transmit one wire frame. Return false only for a fatal output failure
    // that should stop the daemon (so systemd restarts it and reopens the
    // device); best-effort sinks swallow their errors and return true.
    virtual bool send(const std::vector<uint8_t>& frame) = 0;

    // out-of-band command to the ESP32 receiver (see common/protocol.hpp).
    // Only the serial transport carries these; for every other sink it's a
    // no-op, mirroring how the viewer ignores command frames on the wire.
    virtual bool sendCommand(uint8_t cmd, const uint8_t* payload = nullptr,
                             uint16_t len = 0)
    {
        (void)cmd;
        (void)payload;
        (void)len;
        return true;
    }
};
