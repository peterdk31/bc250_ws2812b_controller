#include "hostreq.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol.hpp"

#include "dbglog.hpp"
#include "link.hpp"
#include "util.hpp"

namespace hostreq
{
static const uint32_t REPEAT_MS = 500; // re-ask this often until answered

// Written from the requesting task (the power switch), read and updated from
// the link's writer task, so the whole tuple moves under one short critical
// section — the same pattern dbglog uses for its ring.
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_req = 0;    // outstanding request code; 0 = nothing
static uint32_t g_nonce = 0; // this request's id, echoed by the ack
static bool g_acked = false;
static uint32_t g_lastSend = 0;
static bool g_sent = false; // has this request gone out even once yet

void request(uint8_t req)
{
    uint32_t nonce = millis();

    taskENTER_CRITICAL(&g_mux);
    g_req = req;
    g_nonce = nonce ? nonce : 1; // 0 is reserved for "no request"
    g_acked = false;
    g_sent = false;
    taskEXIT_CRITICAL(&g_mux);
}

void cancel()
{
    taskENTER_CRITICAL(&g_mux);
    g_req = 0;
    taskEXIT_CRITICAL(&g_mux);
}

// aligned single-byte read of a value only ever set to true by another task;
// no lock needed (as dbglog::active())
bool acked() { return g_acked; }

void satisfy(uint8_t req)
{
    taskENTER_CRITICAL(&g_mux);
    if (g_req && g_req == req)
        g_acked = true;
    taskEXIT_CRITICAL(&g_mux);
}

void tick(uint32_t now)
{
    uint8_t req;
    uint32_t nonce;
    bool due;

    taskENTER_CRITICAL(&g_mux);
    req = g_req;
    nonce = g_nonce;
    due = req && !g_acked && (!g_sent || now - g_lastSend >= REPEAT_MS);
    if (due)
    {
        g_lastSend = now;
        g_sent = true;
    }
    taskEXIT_CRITICAL(&g_mux);

    if (!due)
        return;

    // SYNC0 REQ_SYNC req(1) nonce(4) checksum — see protocol.hpp
    uint8_t f[8];
    f[0] = proto::SYNC0;
    f[1] = proto::REQ_SYNC;
    f[2] = req;

    uint8_t sum = req;
    for (int i = 0; i < 4; i++)
    {
        f[3 + i] = (uint8_t)(nonce >> (8 * i));
        sum ^= f[3 + i];
    }
    f[7] = sum;

    // On a USB Serial/JTAG link this drops the frame rather than stalling if
    // nothing is draining the TX buffer (see link::write) — which is exactly
    // the case where no daemon is listening, and the caller's ack timeout is
    // what reports that.
    link::write(f, sizeof f);
}

void onAck(const uint8_t* payload, uint16_t len)
{
    if (len < 5)
        return;

    uint8_t req = payload[0];
    uint32_t nonce = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8)
        | ((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);

    bool match;
    taskENTER_CRITICAL(&g_mux);
    match = g_req && g_req == req && g_nonce == nonce;
    if (match)
        g_acked = true;
    taskEXIT_CRITICAL(&g_mux);

    if (!match)
    {
        // an ack for a request we've already given up on (or never made): the
        // nonce is what keeps it from retiring the *next* one
        dbglog::line("hostreq: stale ack req=%u nonce=%u, ignored", (unsigned)req,
                     (unsigned)nonce);
    }
}
} // namespace hostreq
