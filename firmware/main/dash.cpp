#include "dash.hpp"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "util.hpp"

namespace dash
{
struct Entry
{
    uint8_t* buf;
    uint16_t cap;
    uint16_t len = 0;
    uint32_t seq = 0;
    uint32_t ms = 0; // millis() of the last arrival, 0 = never
};

static uint8_t g_fanCfg[512];
static uint8_t g_fanTel[384];
static uint8_t g_stripCfg[512];

static Entry g_slots[SLOTS] = {
    {g_fanCfg, sizeof g_fanCfg},
    {g_fanTel, sizeof g_fanTel},
    {g_stripCfg, sizeof g_stripCfg},
};

// one lock for all three: the writers are a single task, the readers a
// 250 ms poll, and a copy is a few hundred bytes
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void set(Slot slot, const uint8_t* payload, uint16_t len)
{
    if (slot >= SLOTS)
        return;
    Entry& e = g_slots[slot];
    if (len == 0 || len > e.cap)
        return;

    uint32_t now = millis();
    taskENTER_CRITICAL(&g_mux);
    memcpy(e.buf, payload, len);
    e.len = len;
    e.seq++;
    e.ms = now ? now : 1;
    taskEXIT_CRITICAL(&g_mux);
}

uint16_t get(Slot slot, uint8_t* out, uint16_t max, uint32_t* seq, uint32_t* ageMs)
{
    if (slot >= SLOTS)
        return 0;
    Entry& e = g_slots[slot];

    uint32_t now = millis();
    taskENTER_CRITICAL(&g_mux);
    uint16_t n = e.len <= max ? e.len : 0;
    if (n && out)
        memcpy(out, e.buf, n);
    if (seq)
        *seq = e.seq;
    if (ageMs)
        *ageMs = e.ms ? now - e.ms : 0xFFFFFFFFu;
    taskEXIT_CRITICAL(&g_mux);
    return n;
}
} // namespace dash
