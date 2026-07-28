#include "dbglog.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol.hpp"

#include "link.hpp"
#include "util.hpp"

namespace dbglog
{
static const int SLOTS = 64;           // ring depth (lines)
static const int TEXT_MAX = 100;       // per-line text cap (excl. NUL)
static const uint32_t ACTIVE_MS = 30000; // "host is listening" window

struct Slot
{
    uint32_t seq; // 0 = unused; monotonic, assigned on write
    uint32_t ms;  // millis() when logged
    uint16_t len; // text bytes
    char text[TEXT_MAX];
};

static Slot g_ring[SLOTS];
static uint32_t g_seq = 0;       // last seq assigned
static int g_head = 0;           // next slot to overwrite (oldest lives here)
static uint32_t g_lastDrainMs = 0;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void line(const char* fmt, ...)
{
    char tmp[TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    uint16_t len = (n >= TEXT_MAX) ? TEXT_MAX - 1 : (uint16_t)n;
    uint32_t ms = millis();

    taskENTER_CRITICAL(&g_mux);
    Slot& s = g_ring[g_head];
    s.seq = ++g_seq;
    s.ms = ms;
    s.len = len;
    memcpy(s.text, tmp, len);
    g_head = (g_head + 1) % SLOTS;
    taskEXIT_CRITICAL(&g_mux);
}

bool active()
{
    uint32_t last = g_lastDrainMs; // aligned 32-bit read; no lock needed
    return last != 0 && (millis() - last) < ACTIVE_MS;
}

// build and transmit one LOG_SYNC frame (see protocol.hpp) for a copied slot
static void transmit(const Slot& s)
{
    uint8_t f[2 + 9 + TEXT_MAX + 1];
    size_t p = 0;
    f[p++] = proto::SYNC0;
    f[p++] = proto::LOG_SYNC;

    // seq(4) ms(4) len(1), little-endian; checksum spans these plus the text
    uint8_t hdr[9] = {
        (uint8_t)s.seq,       (uint8_t)(s.seq >> 8),
        (uint8_t)(s.seq >> 16), (uint8_t)(s.seq >> 24),
        (uint8_t)s.ms,        (uint8_t)(s.ms >> 8),
        (uint8_t)(s.ms >> 16), (uint8_t)(s.ms >> 24),
        (uint8_t)s.len};
    uint8_t sum = 0;
    for (int i = 0; i < 9; i++)
    {
        f[p++] = hdr[i];
        sum ^= hdr[i];
    }
    for (uint16_t i = 0; i < s.len; i++)
    {
        f[p++] = (uint8_t)s.text[i];
        sum ^= (uint8_t)s.text[i];
    }
    f[p++] = sum;

    link::write(f, p);
}

void onDrain(uint32_t sinceSeq)
{
    g_lastDrainMs = millis();

    // oldest-to-newest is g_head forward, which is ascending seq (unused slots
    // read seq 0 and are skipped). Copy each slot out under the lock, transmit
    // outside it so a slow write never blocks a logging task.
    for (int k = 0; k < SLOTS; k++)
    {
        int i = (g_head + k) % SLOTS;

        Slot s;
        taskENTER_CRITICAL(&g_mux);
        s = g_ring[i];
        taskEXIT_CRITICAL(&g_mux);

        if (s.seq == 0 || s.seq <= sinceSeq)
            continue;

        transmit(s);
    }
}
} // namespace dbglog
