#include "dash.hpp"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol.hpp"
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

static uint8_t g_fanCfg[proto::DASH_FAN_CONFIG_MAX];
static uint8_t g_fanTel[proto::DASH_FAN_TELEM_MAX];
static uint8_t g_stripCfg[proto::DASH_STRIP_CONFIG_MAX];
static uint8_t g_fanSens[proto::DASH_FAN_SENSORS_MAX];
static uint8_t g_pwrCfg[proto::DASH_PWR_CONFIG_MAX];

static_assert(proto::DASH_FAN_CONFIG_MAX <= MAX_LEN && proto::DASH_FAN_SENSORS_MAX <= MAX_LEN,
              "MAX_LEN is the largest slot");

static Entry g_slots[SLOTS] = {
    {g_fanCfg, sizeof g_fanCfg},
    {g_fanTel, sizeof g_fanTel},
    {g_stripCfg, sizeof g_stripCfg},
    {g_fanSens, sizeof g_fanSens},
    {g_pwrCfg, sizeof g_pwrCfg},
};

// one lock for all of them: the writers are a single task, the readers a
// 250 ms poll and the phone's page reads, and a copy is at most 2 KB
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void set(Slot slot, const uint8_t* payload, uint16_t len)
{
    if (slot >= SLOTS)
        return;
    Entry& e = g_slots[slot];
    // an empty payload clears the slot: the daemon has nothing there (a
    // config with no fans block), which the phone shows as it shows "no daemon"
    if (len > e.cap)
        return;

    uint32_t now = millis();
    taskENTER_CRITICAL(&g_mux);
    if (len)
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
