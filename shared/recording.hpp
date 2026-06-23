#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "protocol.hpp"

// Shared, hardware-free recording logic: the on-wire/on-flash byte layout, the
// streaming accumulator that turns CMD_REC_* frames into a stored recording,
// and the replay stepper. The ESP32 firmware and the host daemon both use
// these, so the format and the playback math can't drift between them — and,
// like shared/receiver.hpp, it all compiles and gets tested on the host even
// though the firmware itself can't be built there.
//
// What stays on the device: LittleFS save/load, the WS2812 output, the UART,
// and millis() (passed into Player::tick). Everything in this header is pure.
namespace rec
{

// one captured animation as a flat pixel buffer: frame i is count*3 bytes at
// frame(i). The bytes are already strip-corrected by the host, so the receiver
// blits them as-is.
struct Recording
{
    uint16_t frameMs = 16;
    uint16_t count = 0;
    uint16_t frameCount = 0;
    uint16_t loopStart = 0; // frame to wrap to when looping (0 = whole thing)
    uint8_t pin = 0;
    bool loop = false;
    std::vector<uint8_t> data;

    static const uint16_t kBeginLen = 15; // CMD_REC_BEGIN payload size
    static const size_t kHeaderLen = 18;  // on-flash header size

    size_t frameBytes() const { return (size_t)count * 3; }

    bool valid() const
    {
        return count && frameCount &&
               data.size() == (size_t)frameCount * frameBytes();
    }

    const uint8_t* frame(uint16_t i) const
    {
        return &data[(size_t)i * frameBytes()];
    }

    void clear()
    {
        frameCount = 0;
        data.clear();
    }

    // FNV-1a over the recording's fields and every pixel byte. Folding the
    // metadata in (not just pixels) means a geometry, timing, loop or
    // loop-point change also changes the hash, so the receiver's skip-unchanged
    // check rewrites flash whenever anything that affects playback differs.
    uint32_t hash() const
    {
        uint32_t h = 2166136261u;
        uint16_t meta[4] = {frameMs, count, loopStart,
                            (uint16_t)(loop ? 1 : 0)};
        for (int m = 0; m < 4; m++)
        {
            h = (h ^ (meta[m] & 0xFF)) * 16777619u;
            h = (h ^ (meta[m] >> 8)) * 16777619u;
        }
        for (size_t i = 0; i < data.size(); i++)
            h = (h ^ data[i]) * 16777619u;
        return h;
    }

    // CMD_REC_BEGIN payload (15 bytes, little-endian; see shared/protocol.hpp):
    //   slot frameMs count pin flags frameCount loopStart hash
    void encodeBegin(uint8_t* out, uint8_t slot) const
    {
        uint32_t h = hash();
        out[0] = slot;
        out[1] = frameMs & 0xFF;     out[2] = frameMs >> 8;
        out[3] = count & 0xFF;       out[4] = count >> 8;
        out[5] = pin;
        out[6] = loop ? proto::REC_FLAG_LOOP : 0;
        out[7] = frameCount & 0xFF;  out[8] = frameCount >> 8;
        out[9] = loopStart & 0xFF;   out[10] = loopStart >> 8;
        out[11] = h; out[12] = h >> 8; out[13] = h >> 16; out[14] = h >> 24;
    }

    // on-flash header (18 bytes): magic + version, then the fields, hash last,
    // one byte reserved
    void encodeHeader(uint8_t* out) const
    {
        uint32_t h = hash();
        out[0] = proto::REC_MAGIC0;
        out[1] = proto::REC_MAGIC1;
        out[2] = proto::REC_VERSION;
        out[3] = frameMs & 0xFF;     out[4] = frameMs >> 8;
        out[5] = count & 0xFF;       out[6] = count >> 8;
        out[7] = frameCount & 0xFF;  out[8] = frameCount >> 8;
        out[9] = loopStart & 0xFF;   out[10] = loopStart >> 8;
        out[11] = pin;
        out[12] = loop ? proto::REC_FLAG_LOOP : 0;
        out[13] = h; out[14] = h >> 8; out[15] = h >> 16; out[16] = h >> 24;
        out[17] = 0;
    }

    // fill geometry/timing from a header (not the pixel data — the caller sizes
    // and reads that itself); false if the magic/version don't match
    bool decodeHeader(const uint8_t* in)
    {
        if (in[0] != proto::REC_MAGIC0 || in[1] != proto::REC_MAGIC1 ||
            in[2] != proto::REC_VERSION)
            return false;

        frameMs = in[3] | (in[4] << 8);
        count = in[5] | (in[6] << 8);
        frameCount = in[7] | (in[8] << 8);
        loopStart = in[9] | (in[10] << 8);
        pin = in[11];
        loop = in[12] & proto::REC_FLAG_LOOP;
        return true;
    }

    // the hash stored in a header, or 0 if it's invalid; lets the receiver
    // compare an incoming upload against a slot's file without reading the
    // whole thing
    static uint32_t headerHash(const uint8_t* in)
    {
        if (in[0] != proto::REC_MAGIC0 || in[1] != proto::REC_MAGIC1 ||
            in[2] != proto::REC_VERSION)
            return 0;

        return (uint32_t)in[13] | ((uint32_t)in[14] << 8) |
               ((uint32_t)in[15] << 16) | ((uint32_t)in[16] << 24);
    }
};

// a decoded CMD_REC_BEGIN header
struct BeginInfo
{
    uint8_t slot = 0;
    uint16_t frameMs = 0, count = 0, frameCount = 0, loopStart = 0;
    uint8_t pin = 0, flags = 0;
    uint32_t hash = 0;

    bool decode(const uint8_t* p, uint16_t len)
    {
        if (len < Recording::kBeginLen)
            return false;

        slot = p[0];
        frameMs = p[1] | (p[2] << 8);
        count = p[3] | (p[4] << 8);
        pin = p[5];
        flags = p[6];
        frameCount = p[7] | (p[8] << 8);
        loopStart = p[9] | (p[10] << 8);
        hash = (uint32_t)p[11] | ((uint32_t)p[12] << 8) |
               ((uint32_t)p[13] << 16) | ((uint32_t)p[14] << 24);
        return true;
    }
};

// Accumulates a streamed recording (CMD_REC_BEGIN/FRAME/END) into RAM, ready
// to commit to flash on completion. The device decides `skip` (incoming hash
// == the slot's stored hash) and passes it to begin(); a skipped upload is
// dropped so no flash write happens. maxBytes bounds the buffer so a corrupt
// header can't trigger a huge allocation.
struct RecordingReceiver
{
    Recording rec;
    bool storing = false;
    size_t maxBytes = 512u * 1024u;

    void reset()
    {
        storing = false;
        rec.clear();
    }

    void begin(const BeginInfo& bi, bool skip)
    {
        reset();

        if (skip)
            return;

        size_t bytes = (size_t)bi.frameCount * bi.count * 3;
        if (!bi.count || !bi.frameCount || bytes > maxBytes)
            return; // bogus geometry/size; ignore the stream (storing stays false)

        rec.frameMs = bi.frameMs;
        rec.count = bi.count;
        rec.pin = bi.pin;
        rec.loop = bi.flags & proto::REC_FLAG_LOOP;
        rec.frameCount = bi.frameCount;
        rec.loopStart = bi.loopStart < bi.frameCount ? bi.loopStart : 0;
        rec.data.clear();
        rec.data.reserve(bytes);
        storing = true;
    }

    void frame(const uint8_t* p, uint16_t len)
    {
        if (!storing || len != rec.frameBytes())
            return;

        if (rec.data.size() + len > (size_t)rec.frameCount * rec.frameBytes())
            return; // more frames than the header declared

        rec.data.insert(rec.data.end(), p, p + len);
    }

    // true once every declared frame has arrived and the buffer is consistent
    bool complete() const { return storing && rec.valid(); }
};

// Replays a Recording one frame per frameMs. tick(now) returns the pixels to
// show this instant, or nullptr if it isn't time yet (or playback is over). A
// looping recording wraps; a finite one shows its last frame once and then
// reports done() — the device holds that frame (and, for shutdown, blanks).
struct Player
{
    void start(const Recording* r)
    {
        rec_ = r;
        frame_ = 0;
        lastMs_ = 0;
        started_ = false;
        done_ = false;
    }

    void stop()
    {
        rec_ = nullptr;
        done_ = false;
    }

    bool active() const { return rec_ && rec_->valid(); }
    bool done() const { return done_; }

    const uint8_t* tick(uint32_t now)
    {
        if (!active() || done_)
            return nullptr;

        if (started_ && now - lastMs_ < rec_->frameMs)
            return nullptr;

        started_ = true;
        lastMs_ = now;

        const uint8_t* px = rec_->frame(frame_);

        if (frame_ + 1 < rec_->frameCount)
            frame_++;
        else if (rec_->loop)
            // wrap to the loop point — the start of the final segment, so a
            // one-shot intro leads into a looping tail (0 = loop the whole thing)
            frame_ = rec_->loopStart < rec_->frameCount ? rec_->loopStart : 0;
        else
            done_ = true; // hold the last frame

        return px;
    }

private:
    const Recording* rec_ = nullptr;
    uint16_t frame_ = 0;
    uint32_t lastMs_ = 0;
    bool started_ = false;
    bool done_ = false;
};

} // namespace rec
