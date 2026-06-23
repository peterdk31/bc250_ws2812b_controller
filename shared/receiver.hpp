#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "protocol.hpp"

// The decode counterpart to host/sink.hpp's Sink. A Sink emits finished wire
// frames (serial out, socket out); a Receiver is handed raw bytes, recognizes
// complete frames, and hands them to a FrameHandler that does something with
// them — drive a real WS2812 strip (the ESP32 firmware) or paint an on-screen
// preview (host/virtual_strip.cpp). Both consumers used to re-implement the
// AA-55 framing and checksum; this is the one shared parser, so they can't
// drift on the wire format any more than Sink and protocol.hpp let them.
namespace proto
{

// What a Receiver calls when a complete, checksum-valid frame arrives.
// Implement onPixels to light LEDs; onCommand is optional — the viewer
// ignores commands, the firmware acts on them (see protocol.hpp).
struct FrameHandler
{
    virtual ~FrameHandler() = default;

    // a validated pixel frame: `count` LEDs as RGB triples at
    // rgb[0 .. count*3), for the strip on data pin `pin`. The pointer is into
    // the Receiver's buffer and is valid only for the duration of the call.
    virtual void onPixels(uint8_t pin, uint16_t count, const uint8_t* rgb) = 0;

    // a validated command frame (protocol.hpp). `payload` is NUL-terminated
    // (one spare byte past `len`) so string payloads need no copy. Default:
    // ignore, mirroring how the viewer skips command frames on the wire.
    virtual void onCommand(uint8_t cmd, const uint8_t* payload, uint16_t len)
    {
        (void)cmd;
        (void)payload;
        (void)len;
    }
};

// Streaming decoder for the host->receiver wire protocol (protocol.hpp). Feed
// it bytes as they arrive — a UART chunk, a whole datagram, a single byte —
// and it carries parse state across feed() calls, so a frame split over two
// reads still decodes. Pure: no I/O and no transport assumptions.
//
// A bad checksum or an out-of-range count/length drops the frame and rescans
// from the next byte, exactly like a receiver resyncing on a noisy line.
class Receiver
{
public:
    // maxLeds bounds the work buffer (maxLeds*3 bytes): a pixel frame claiming
    // more LEDs, or a command with a longer payload, is rejected as noise. The
    // buffer carries one spare byte so onCommand's payload can be NUL-terminated.
    explicit Receiver(FrameHandler& handler, uint16_t maxLeds = 2048)
        : handler_(handler),
          cap_(static_cast<size_t>(maxLeds) * 3),
          buf_(cap_ + 1)
    {
    }

    // push received bytes through the state machine
    void feed(const uint8_t* data, size_t n)
    {
        for (size_t i = 0; i < n; i++)
            step(data[i]);
    }

    // bytes consumed since the last valid frame. The firmware watches this to
    // decide the host is talking at another baud and it should hunt.
    uint32_t bytesSinceValid() const { return bytesSinceValid_; }

    // drop any half-decoded frame and zero the counter. The firmware calls
    // this right after switching baud, so a partial frame at the old rate
    // can't poison the scan at the new one.
    void reset()
    {
        state_ = SCAN;
        have_ = 0;
        need_ = 0;
        bytesSinceValid_ = 0;
    }

private:
    enum State
    {
        SCAN,     // hunting for SYNC0
        TYPE,     // have SYNC0, reading the frame-type byte
        PIX_HDR,  // pixel frame: reading pin + 16-bit LED count
        PIX_DATA, // pixel frame: reading the RGB bytes
        PIX_SUM,  // pixel frame: reading the checksum
        CMD_HDR,  // command frame: reading cmd + 16-bit length
        CMD_DATA, // command frame: reading the payload
        CMD_SUM   // command frame: reading the checksum
    };

    void step(uint8_t b)
    {
        bytesSinceValid_++;

        switch (state_)
        {
        case SCAN:
            // every frame starts with SYNC0; anything else is line noise
            if (b == SYNC0)
                state_ = TYPE;
            break;

        case TYPE:
            // the second byte selects the frame type. Anything else (noise,
            // or a stray SYNC0) drops us back to scanning — the same resync
            // the byte-at-a-time firmware loop did.
            if (b == SYNC1)
            {
                state_ = PIX_HDR;
                have_ = 0;
            }
            else if (b == CMD_SYNC)
            {
                state_ = CMD_HDR;
                have_ = 0;
            }
            else
            {
                state_ = SCAN;
            }
            break;

        case PIX_HDR:
            hdr_[have_++] = b;
            if (have_ == 3)
            {
                pin_ = hdr_[0];
                count_ = (uint16_t)(hdr_[1] | (hdr_[2] << 8));

                // zero or too-many LEDs: a header-shaped run of noise. Rescan.
                if (count_ == 0 || (size_t)count_ * 3 > cap_)
                {
                    state_ = SCAN;
                    break;
                }

                need_ = (size_t)count_ * 3;
                have_ = 0;
                sum_ = hdr_[0] ^ hdr_[1] ^ hdr_[2];
                state_ = PIX_DATA;
            }
            break;

        case PIX_DATA:
            buf_[have_++] = b;
            sum_ ^= b;
            if (have_ == need_)
                state_ = PIX_SUM;
            break;

        case PIX_SUM:
            // XOR of pin, count and pixels; a mismatch means we latched onto
            // data that looked like a header — drop it and rescan
            if (b == sum_)
            {
                bytesSinceValid_ = 0;
                handler_.onPixels(pin_, count_, buf_.data());
            }
            state_ = SCAN;
            break;

        case CMD_HDR:
            hdr_[have_++] = b;
            if (have_ == 3)
            {
                cmd_ = hdr_[0];
                len_ = (uint16_t)(hdr_[1] | (hdr_[2] << 8));

                // a bogus length is noise; rescan rather than wait for bytes
                // that will never come
                if (len_ > cap_)
                {
                    state_ = SCAN;
                    break;
                }

                need_ = len_;
                have_ = 0;
                sum_ = hdr_[0] ^ hdr_[1] ^ hdr_[2];
                state_ = (len_ == 0) ? CMD_SUM : CMD_DATA;
            }
            break;

        case CMD_DATA:
            buf_[have_++] = b;
            sum_ ^= b;
            if (have_ == need_)
                state_ = CMD_SUM;
            break;

        case CMD_SUM:
            if (b == sum_)
            {
                bytesSinceValid_ = 0;
                buf_[len_] = 0; // NUL-terminate for string payloads (see header)
                handler_.onCommand(cmd_, buf_.data(), len_);
            }
            state_ = SCAN;
            break;
        }
    }

    FrameHandler& handler_;
    const size_t cap_;        // max payload bytes (maxLeds*3)
    std::vector<uint8_t> buf_; // cap_ + 1, the extra byte for the NUL above

    State state_ = SCAN;
    uint8_t hdr_[3];          // pin/count or cmd/len, filled in *_HDR
    size_t have_ = 0;         // bytes of the current field gathered so far
    size_t need_ = 0;         // bytes expected in the current data run
    uint8_t sum_ = 0;         // running checksum over header + data

    uint8_t pin_ = 0;
    uint16_t count_ = 0;
    uint8_t cmd_ = 0;
    uint16_t len_ = 0;

    uint32_t bytesSinceValid_ = 0;
};

} // namespace proto
