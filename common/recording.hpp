#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "protocol.hpp"

// Shared, hardware-free recording logic: the on-wire/on-flash byte layout, the
// streaming accumulator that turns CMD_REC_* frames into a stored recording,
// and the replay stepper. The ESP32 firmware and the host daemon both use
// these, so the format and the playback math can't drift between them — and,
// like common/receiver.hpp, it all compiles and gets tested on the host even
// though the firmware itself can't be built there.
//
// What stays on the device: LittleFS save/load, the WS2812 output, the UART,
// and millis() (passed into Player::tick). Everything in this header is pure.
namespace rec
{

// one captured animation as a flat pixel buffer: frame i is count*6 bytes at
// frame(i) — per LED three little-endian 8.8 fixed-point channel values, the
// same depth the live deep pixel frame carries (common/protocol.hpp). The
// values are already strip-corrected by the host; the receiver replays them
// through the same dithered refresh as live frames.
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

    size_t frameBytes() const { return (size_t)count * 6; }

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
    //
    // Split into meta seed + byte folding so a device that never holds the
    // pixels (it streams an upload straight to flash) can still compute the
    // same hash incrementally, frame by frame (see RecordingReceiver).
    static uint32_t hashMeta(uint16_t frameMs, uint16_t count,
                             uint16_t loopStart, bool loop)
    {
        uint32_t h = 2166136261u;
        uint16_t meta[4] = {frameMs, count, loopStart,
                            (uint16_t)(loop ? 1 : 0)};
        for (int m = 0; m < 4; m++)
        {
            h = (h ^ (meta[m] & 0xFF)) * 16777619u;
            h = (h ^ (meta[m] >> 8)) * 16777619u;
        }
        return h;
    }

    static uint32_t hashFold(uint32_t h, const uint8_t* p, size_t n)
    {
        for (size_t i = 0; i < n; i++)
            h = (h ^ p[i]) * 16777619u;
        return h;
    }

    uint32_t hash() const
    {
        return hashFold(hashMeta(frameMs, count, loopStart, loop),
                        data.data(), data.size());
    }

    // CMD_REC_BEGIN payload (15 bytes, little-endian; see common/protocol.hpp):
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
    // one byte reserved. The overload takes a caller-supplied hash — for a
    // device streaming an upload to flash, which never holds the pixels and
    // so can't recompute it here (it verifies the stream against this hash
    // incrementally instead; see RecordingReceiver).
    void encodeHeader(uint8_t* out) const { encodeHeader(out, hash()); }

    void encodeHeader(uint8_t* out, uint32_t h) const
    {
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

// Bookkeeping for a streamed recording (CMD_REC_BEGIN/FRAME/END): vets the
// BEGIN geometry, size-checks and counts each FRAME, and says when every
// declared frame has arrived intact. Where the bytes go is the caller's
// business — the device appends each vetted frame straight to flash (see the
// firmware's rec_store) rather than accumulating ~100 KB of pixels here,
// which is exactly the allocation a small chip can't be trusted to make.
//
// Integrity is end-to-end even so: begin() seeds the shared FNV with the
// stream's metadata and frame() folds every pixel byte in, so complete()
// only reports true when the recomputed hash matches what the host declared
// — a stream with the right byte count but wrong bytes is refused just as
// the old buffer-then-recompute path refused it.
//
// The device decides `skip` (incoming hash == the slot's stored hash) and
// passes it to begin(); a skipped upload is dropped so no flash write
// happens. maxBytes bounds the total a corrupt header can declare.
struct RecordingReceiver
{
    Recording meta;       // geometry/timing from BEGIN; data stays empty
    bool storing = false;
    uint16_t framesSeen = 0;
    size_t maxBytes = 512u * 1024u;

    void reset()
    {
        storing = false;
        framesSeen = 0;
        hash_ = 0;
        want_ = 0;
        meta = Recording();
    }

    // true when the stream should be stored (the caller opens its sink)
    bool begin(const BeginInfo& bi, bool skip)
    {
        reset();

        if (skip)
            return false;

        size_t bytes = (size_t)bi.frameCount * bi.count * 6;
        if (!bi.count || !bi.frameCount || bytes > maxBytes)
            return false; // bogus geometry/size; ignore the stream

        meta.frameMs = bi.frameMs;
        meta.count = bi.count;
        meta.pin = bi.pin;
        meta.loop = bi.flags & proto::REC_FLAG_LOOP;
        meta.frameCount = bi.frameCount;
        meta.loopStart = bi.loopStart < bi.frameCount ? bi.loopStart : 0;
        hash_ = Recording::hashMeta(meta.frameMs, meta.count, meta.loopStart,
                                    meta.loop);
        want_ = bi.hash;
        storing = true;
        return true;
    }

    // true when this FRAME payload belongs to the stream and should be stored
    bool frame(const uint8_t* p, uint16_t len)
    {
        if (!storing || len != meta.frameBytes())
            return false;

        if (framesSeen >= meta.frameCount)
            return false; // more frames than the header declared

        hash_ = Recording::hashFold(hash_, p, len);
        framesSeen++;
        return true;
    }

    // every declared frame arrived and the bytes hash to what BEGIN declared
    bool complete() const
    {
        return storing && framesSeen == meta.frameCount && hash_ == want_;
    }

private:
    uint32_t hash_ = 0; // FNV over meta + the frames seen so far
    uint32_t want_ = 0; // the hash BEGIN declared for the whole recording
};

// Replays a Recording one frame per frameMs, pacing off its header fields
// alone. tickIndex(now) returns the frame to show this instant, or -1 if it
// isn't time yet (or playback is over) — for a caller that fetches the frame
// bytes itself (the device streams them from flash, one frame per tick,
// instead of holding the whole recording in RAM). tick(now) is the in-RAM
// convenience over it: same pacing, returning a pointer into data (the
// daemon's --preview). A looping recording wraps; a finite one shows its
// last frame once and then reports done() — the device holds that frame
// (and, for shutdown, blanks).
struct Player
{
    // r must stay alive while playing; only its header fields are read here,
    // so a meta-only Recording (empty data) works with tickIndex()
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

    bool active() const { return rec_ && rec_->count && rec_->frameCount; }
    bool done() const { return done_; }

    int tickIndex(uint32_t now)
    {
        if (!active() || done_)
            return -1;

        if (started_ && now - lastMs_ < rec_->frameMs)
            return -1;

        started_ = true;
        lastMs_ = now;

        int idx = frame_;

        if (frame_ + 1 < rec_->frameCount)
            frame_++;
        else if (rec_->loop)
            // wrap to the loop point — the start of the final segment, so a
            // one-shot intro leads into a looping tail (0 = loop the whole thing)
            frame_ = rec_->loopStart < rec_->frameCount ? rec_->loopStart : 0;
        else
            done_ = true; // hold the last frame

        return idx;
    }

    const uint8_t* tick(uint32_t now)
    {
        if (!rec_ || !rec_->valid())
            return nullptr;

        int idx = tickIndex(now);
        return idx < 0 ? nullptr : rec_->frame((uint16_t)idx);
    }

private:
    const Recording* rec_ = nullptr;
    uint16_t frame_ = 0;
    uint32_t lastMs_ = 0;
    bool started_ = false;
    bool done_ = false;
};

} // namespace rec
