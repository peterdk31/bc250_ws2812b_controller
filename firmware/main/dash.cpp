#include "dash.hpp"

#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dbglog.hpp"
#include "util.hpp"

namespace dash
{
struct Entry
{
    uint8_t* buf = nullptr;
    uint16_t cap = 0;
    uint16_t len = 0;
    uint32_t seq = 0;
    uint32_t ms = 0; // millis() of the last arrival, 0 = never
};

static Entry g_views[VIEWS];

// buffers grow in steps of this, so a view whose length wanders by a few
// bytes from one arrival to the next (the telemetry) settles on one size
static const uint16_t GROW = 256;

// one lock for all of them: the writer is a single task, the readers a
// 250 ms poll and the phone's page reads, and a copy is at most 2 KB
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void set(uint8_t view, const uint8_t* payload, uint16_t len)
{
    if (view >= VIEWS || len > MAX_LEN)
        return;
    Entry& e = g_views[view];

    // a bigger buffer is filled before anyone can see it (no allocating in a
    // critical section), then swapped in whole with the length; only this
    // task ever grows one, so cap can't move meanwhile
    uint8_t* grown = nullptr;
    uint16_t cap = e.cap;
    if (len > e.cap)
    {
        cap = (uint16_t)((len + GROW - 1) / GROW * GROW);
        if (cap > MAX_LEN)
            cap = MAX_LEN;
        grown = (uint8_t*)malloc(cap);
        if (!grown)
        {
            dbglog::line("dash: no heap for view %u (%u bytes) — kept the last one", (unsigned)view,
                         (unsigned)len);
            return;
        }
        memcpy(grown, payload, len);
    }

    uint32_t now = millis();
    uint8_t* old = nullptr;
    taskENTER_CRITICAL(&g_mux);
    if (grown)
    {
        old = e.buf;
        e.buf = grown;
        e.cap = cap;
    }
    else if (len)
        memcpy(e.buf, payload, len);
    e.len = len;
    e.seq++;
    e.ms = now ? now : 1;
    taskEXIT_CRITICAL(&g_mux);
    free(old);
}

uint16_t get(uint8_t view, uint8_t* out, uint16_t max, uint16_t* len, uint32_t* seq,
             uint32_t* ageMs)
{
    if (view >= VIEWS)
        return 0;
    Entry& e = g_views[view];

    uint32_t now = millis();
    taskENTER_CRITICAL(&g_mux);
    uint16_t n = out ? (e.len < max ? e.len : max) : 0;
    if (n)
        memcpy(out, e.buf, n);
    if (len)
        *len = e.len;
    if (seq)
        *seq = e.seq;
    if (ageMs)
        *ageMs = e.ms ? now - e.ms : 0xFFFFFFFFu;
    taskEXIT_CRITICAL(&g_mux);
    return n;
}
} // namespace dash
