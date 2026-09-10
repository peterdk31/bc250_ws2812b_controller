#include "hostreq.hpp"

#include <cstring>

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

// the message queue (post/tick): a few fixed slots, oldest out first. Sized
// for the dashboard's traffic — a config edit is ~200 bytes and rare, a watch
// keepalive one byte every ten seconds — so a full queue means the link task
// has stalled, not that the phone is fast.
static const int MSG_SLOTS = 3;
struct Msg
{
    uint8_t kind;
    uint16_t len;
    uint8_t payload[MSG_MAX];
};
static Msg g_msgs[MSG_SLOTS];
static int g_msgHead = 0;  // next slot to send
static int g_msgCount = 0; // queued slots

bool post(uint8_t kind, const uint8_t* payload, uint16_t len)
{
    if (len > MSG_MAX)
        return false;

    bool ok;
    taskENTER_CRITICAL(&g_mux);
    ok = g_msgCount < MSG_SLOTS;
    if (ok)
    {
        Msg& m = g_msgs[(g_msgHead + g_msgCount) % MSG_SLOTS];
        m.kind = kind;
        m.len = len;
        if (len)
            memcpy(m.payload, payload, len);
        g_msgCount++;
    }
    taskEXIT_CRITICAL(&g_mux);
    return ok;
}

// transmit the oldest queued message, if any (tick's second half)
static void sendMessage()
{
    // static, not stack: only the link task ever runs this, and a queued
    // config edit is a couple of hundred bytes the led_rx stack needn't carry
    static Msg m;
    static uint8_t f[4 + MSG_MAX + 1];
    bool have;
    taskENTER_CRITICAL(&g_mux);
    have = g_msgCount > 0;
    if (have)
    {
        m = g_msgs[g_msgHead];
        g_msgHead = (g_msgHead + 1) % MSG_SLOTS;
        g_msgCount--;
    }
    taskEXIT_CRITICAL(&g_mux);

    if (!have)
        return;

    // SYNC0 MSG_SYNC kind(1) len(1) payload checksum — see protocol.hpp
    f[0] = proto::SYNC0;
    f[1] = proto::MSG_SYNC;
    f[2] = m.kind;
    f[3] = (uint8_t)m.len;
    uint8_t sum = m.kind ^ (uint8_t)m.len;
    for (uint16_t i = 0; i < m.len; i++)
    {
        f[4 + i] = m.payload[i];
        sum ^= m.payload[i];
    }
    f[4 + m.len] = sum;
    link::write(f, 5 + m.len);
}

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
    sendMessage();

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
