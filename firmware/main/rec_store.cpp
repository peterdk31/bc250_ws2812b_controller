#include "rec_store.hpp"

#include <cstdio>

#include "protocol.hpp"

#include "render.hpp" // MAX_LEDS bounds a loadable recording

#define REC_PATH_POWER_ON LFS_BASE "/boot.rec"
#define REC_PATH_SHUTDOWN LFS_BASE "/shutdown.rec"

namespace rec_store
{
static const char* recPath(uint8_t slot)
{
    return slot == proto::SLOT_SHUTDOWN ? REC_PATH_SHUTDOWN : REC_PATH_POWER_ON;
}

bool load(uint8_t slot, rec::Recording& out)
{
    FILE* f = fopen(recPath(slot), "rb");
    if (!f)
        return false;

    uint8_t hdr[rec::Recording::kHeaderLen];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || !out.decodeHeader(hdr))
    {
        fclose(f);
        return false;
    }

    size_t bytes = (size_t)out.frameCount * out.frameBytes();
    if (out.count == 0 || out.count > MAX_LEDS || out.frameCount == 0 ||
        bytes > REC_MAX_BYTES)
    {
        fclose(f);
        return false;
    }

    out.data.resize(bytes);
    bool ok = fread(out.data.data(), 1, bytes, f) == bytes;
    fclose(f);

    if (!ok)
    {
        out.clear();
        return false;
    }

    return true;
}

void save(uint8_t slot, const rec::Recording& r)
{
    // write to a temp file and rename over the slot only if every byte
    // landed: LittleFS's rename is atomic, so an interrupted write (power
    // cut, esptool reset mid-upload) can never leave a valid-looking header
    // over truncated data — the slot keeps its previous recording instead
    char tmp[40];
    snprintf(tmp, sizeof tmp, "%s.tmp", recPath(slot));

    FILE* f = fopen(tmp, "wb");
    if (!f)
        return;

    uint8_t hdr[rec::Recording::kHeaderLen];
    r.encodeHeader(hdr);
    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;

    if (ok && !r.data.empty())
        ok = fwrite(r.data.data(), 1, r.data.size(), f) == r.data.size();

    ok = (fclose(f) == 0) && ok;

    if (ok)
        rename(tmp, recPath(slot));
    else
        remove(tmp);
}

uint32_t storedHash(uint8_t slot)
{
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
