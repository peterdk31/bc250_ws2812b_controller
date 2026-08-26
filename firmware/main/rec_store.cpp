#include "rec_store.hpp"

#include <cstdio>

#include "protocol.hpp"

#define REC_PATH_POWER_ON LFS_BASE "/boot.rec"
#define REC_PATH_SHUTDOWN LFS_BASE "/shutdown.rec"

namespace rec_store
{
static const char* recPath(uint8_t slot)
{
    return slot == proto::SLOT_SHUTDOWN ? REC_PATH_SHUTDOWN : REC_PATH_POWER_ON;
}

// --- streamed upload ---
// one at a time: the host sends its recordings back to back, never interleaved
static FILE* wf = nullptr;
static uint8_t wfSlot = 0;
static bool wfOk = false;

bool saveBegin(uint8_t slot, const rec::Recording& meta, uint32_t hash)
{
    saveAbort(); // a dangling earlier stream (interrupted mid-upload)

    char tmp[40];
    snprintf(tmp, sizeof tmp, "%s.tmp", recPath(slot));

    wf = fopen(tmp, "wb");
    if (!wf)
        return false; // filesystem not mounted, or out of space

    uint8_t hdr[rec::Recording::kHeaderLen];
    meta.encodeHeader(hdr, hash);

    wfSlot = slot;
    wfOk = fwrite(hdr, 1, sizeof hdr, wf) == sizeof hdr;
    return wfOk;
}

void saveFrame(const uint8_t* px, size_t len)
{
    if (!wf || !wfOk)
        return;

    // a failed write latches wfOk so commit refuses the truncated file; keep
    // consuming (and dropping) the rest of the stream rather than erroring
    // per frame
    wfOk = fwrite(px, 1, len, wf) == len;
}

bool saveCommit()
{
    if (!wf)
        return false;

    bool ok = (fclose(wf) == 0) && wfOk;
    wf = nullptr;

    char tmp[40];
    snprintf(tmp, sizeof tmp, "%s.tmp", recPath(wfSlot));

    if (ok)
        ok = rename(tmp, recPath(wfSlot)) == 0;
    else
        remove(tmp);

    return ok;
}

void saveAbort()
{
    if (!wf)
        return;

    fclose(wf);
    wf = nullptr;

    char tmp[40];
    snprintf(tmp, sizeof tmp, "%s.tmp", recPath(wfSlot));
    remove(tmp);
}

// --- streamed replay ---
static FILE* pf = nullptr;
static int pfSlot = -1;
static size_t pfFrameBytes = 0;
static uint16_t pfFrameCount = 0;
static uint32_t pfHash = 0; // the open file's stored hash (see storedHash)

bool playOpen(uint8_t slot, rec::Recording& meta, uint16_t maxCount)
{
    playClose();

    FILE* f = fopen(recPath(slot), "rb");
    if (!f)
        return false;

    uint8_t hdr[rec::Recording::kHeaderLen];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || !meta.decodeHeader(hdr))
    {
        fclose(f);
        return false;
    }

    size_t bytes = (size_t)meta.frameCount * meta.frameBytes();
    if (meta.count == 0 || meta.count > maxCount || meta.frameCount == 0 ||
        bytes > REC_MAX_BYTES)
    {
        fclose(f);
        return false;
    }

    // the file must hold exactly what its header declares; anything else is
    // truncation or trailing garbage, and replaying it would show junk
    if (fseek(f, 0, SEEK_END) != 0 || ftell(f) != (long)(sizeof hdr + bytes))
    {
        fclose(f);
        return false;
    }

    pf = f;
    pfSlot = slot;
    pfFrameBytes = meta.frameBytes();
    pfFrameCount = meta.frameCount;
    pfHash = rec::Recording::headerHash(hdr);
    return true;
}

bool playRead(uint16_t frame, uint8_t* out, size_t len)
{
    if (!pf || frame >= pfFrameCount || len != pfFrameBytes)
        return false;

    long off = (long)(rec::Recording::kHeaderLen + (size_t)frame * pfFrameBytes);
    return fseek(pf, off, SEEK_SET) == 0 && fread(out, 1, len, pf) == len;
}

void playClose()
{
    if (!pf)
        return;

    fclose(pf);
    pf = nullptr;
    pfSlot = -1;
}

int playSlot()
{
    return pf ? pfSlot : -1;
}

uint32_t storedHash(uint8_t slot)
{
    // LittleFS doesn't support opening a file twice: if the slot's file is
    // the one being replayed right now (boot replay while the daemon
    // re-uploads), answer from the hash cached when the replay opened it
    if (pf && pfSlot == slot)
        return pfHash;

    FILE* f = fopen(recPath(slot), "rb");
    if (!f)
        return 0;

    uint8_t hdr[rec::Recording::kHeaderLen];
    uint32_t h = 0;

    if (fread(hdr, 1, sizeof hdr, f) == sizeof hdr)
    {
        rec::Recording meta;
        if (meta.decodeHeader(hdr) && fseek(f, 0, SEEK_END) == 0 &&
            ftell(f) == (long)(sizeof hdr +
                               (size_t)meta.frameCount * meta.frameBytes()))
            h = rec::Recording::headerHash(hdr);
    }

    fclose(f);
    return h;
}
} // namespace rec_store
