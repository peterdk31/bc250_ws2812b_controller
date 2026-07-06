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

    // a validated pixel frame: `count` LEDs as RGB triples of 8.8 fixed-point
    // channel values (strip code × 256, 0..0xFF00) at rgb[0 .. count*3), for
    // the strip on data pin `pin`. The parser normalizes both wire depths to
    // this — a deep frame decodes as sent, a plain 8-bit frame is widened by
    // <<8 — so a handler has one code path and an older daemon still works.
    // `anim` ids the animation producing the frame and `xms` is the crossfade
    // duration — when `anim` differs from the previous frame, dissolve this
    // one in over the last shown across `xms` ms (see common/fade.hpp). The
    // pointer is into the Receiver's buffer and is valid only for the
    // duration of the call.
    virtual void onPixels(uint8_t pin, uint16_t count, uint16_t anim,
                          uint16_t xms, const uint16_t* rgb) = 0;

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
    // maxLeds bounds the work buffers (maxLeds*6 raw bytes — a deep frame's
    // worst case — plus the decoded 8.8 pixels): a pixel frame claiming more
    // LEDs, or a command with a longer payload, is rejected as noise. The raw
    // buffer carries one spare byte so onCommand's payload can be
    // NUL-terminated.
    explicit Receiver(FrameHandler& handler, uint16_t maxLeds = 2048)
        : handler_(handler),
          maxLeds_(maxLeds),
          cap_(static_cast<size_t>(maxLeds) * 6),
          buf_(cap_ + 1),
          px_(static_cast<size_t>(maxLeds) * 3)
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
            if (b == SYNC1 || b == SYNC1_16)
            {
                deep_ = (b == SYNC1_16);
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
            // pin, count(2), anim(2), xms(2) — see protocol.hpp's PIX_HEADER
            hdr_[have_++] = b;
            if (have_ == 7)
            {
                pin_ = hdr_[0];
                count_ = (uint16_t)(hdr_[1] | (hdr_[2] << 8));
                anim_ = (uint16_t)(hdr_[3] | (hdr_[4] << 8));
                xms_ = (uint16_t)(hdr_[5] | (hdr_[6] << 8));

                // zero or too-many LEDs: a header-shaped run of noise. Rescan.
                if (count_ == 0 || count_ > maxLeds_)
                {
                    state_ = SCAN;
                    break;
                }

                need_ = (size_t)count_ * (deep_ ? 6 : 3);
                have_ = 0;
                sum_ = hdr_[0] ^ hdr_[1] ^ hdr_[2] ^ hdr_[3] ^ hdr_[4]
                       ^ hdr_[5] ^ hdr_[6];
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
            // XOR of header and pixel bytes; a mismatch means we latched onto
            // data that looked like a header — drop it and rescan
            if (b == sum_)
            {
                bytesSinceValid_ = 0;

                // normalize to 8.8: a deep frame decodes its little-endian
                // pairs, a plain 8-bit frame widens (v<<8 — the LUT's own
                // scaling, so both depths mean the same light)
                size_t n = (size_t)count_ * 3;

                if (deep_)
                    for (size_t i = 0; i < n; i++)
                        px_[i] = (uint16_t)(buf_[i * 2]
                                            | (buf_[i * 2 + 1] << 8));
                else
                    for (size_t i = 0; i < n; i++)
                        px_[i] = (uint16_t)(buf_[i] << 8);

                handler_.onPixels(pin_, count_, anim_, xms_, px_.data());
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
    const uint16_t maxLeds_;
    const size_t cap_;         // max payload bytes (maxLeds*6, a deep frame)
    std::vector<uint8_t> buf_; // cap_ + 1, the extra byte for the NUL above
    std::vector<uint16_t> px_; // maxLeds*3 decoded 8.8 pixels for onPixels

    State state_ = SCAN;
    uint8_t hdr_[7];          // pixel header (7) or cmd/len (3), filled in *_HDR
    size_t have_ = 0;         // bytes of the current field gathered so far
    size_t need_ = 0;         // bytes expected in the current data run
    uint8_t sum_ = 0;         // running checksum over header + data
    bool deep_ = false;       // current pixel frame is 8.8 (SYNC1_16)

    uint8_t pin_ = 0;
    uint16_t count_ = 0;
    uint16_t anim_ = 0;
    uint16_t xms_ = 0;
    uint8_t cmd_ = 0;
    uint16_t len_ = 0;

    uint32_t bytesSinceValid_ = 0;
};

} // namespace proto
