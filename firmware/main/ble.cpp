#include "ble.hpp"

#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_partition.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "dbglog.hpp"
#include "power_switch.hpp"
#include "util.hpp"

#define BLOG(fmt, ...) dbglog::line("ble: " fmt, ##__VA_ARGS__)

// The GATT surface — one custom service, two characteristics:
//
//   control (write):        token(16) op(1). Token is the flash-time shared
//                           secret, byte-for-byte (short tokens NUL-padded —
//                           the web page pads the same way). op 0x01 = power
//                           on, 0x02 = graceful shutdown, 0x03 = hard off
//                           (release PS_ON#: the remote form of holding the
//                           button, for a crashed machine). A wrong token is
//                           rejected with "insufficient authentication"; a
//                           right one stages the request with the pwr task
//                           and succeeds even if the state makes it moot
//                           (the status characteristic is how a client sees
//                           what actually happened).
//   status (read + notify): one byte, pwr's coarse PSU state: 0 = off,
//                           1 = booting, 2 = on. Notifies on change, so the
//                           phone watches the power-on it asked for confirm
//                           itself via the sense wire.
//
// The 128-bit UUIDs are this project's own (random base, "bc250" spelled
// into the tail); the web page must list the service UUID to find us.
// NimBLE wants them little-endian:
//   service a5f20001-8f11-4e0e-9b3a-0bc250e0c001, control ...0002, status ...0003
namespace ble
{
static const uint32_t POLL_MS = 250; // policy task cadence

// Advertising runs in both PSU states — a phone must be able to reach a
// machine that crashed, which is precisely when the host is "up" — but at
// two paces: quick to find while the machine is off (the everyday power-on
// case, and 300 ms is still gentle on the 5VSB budget), slow while it is on,
// where reaching us is the rare rescue case and every radio interrupt is one
// the RMT strip refills don't need to compete with.
static const uint16_t ADV_ITVL_OFF = 0x01E0; // 480 × 0.625 ms = 300 ms
static const uint16_t ADV_ITVL_ON = 0x0800;  // 2048 × 0.625 ms = 1.28 s

static const uint16_t TOKEN_LEN = 16;
static const uint16_t NAME_LEN = 16;

static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x01, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t CTRL_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x02, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t STAT_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x03, 0x00, 0xf2, 0xa5);

static const uint8_t OP_POWER_ON = 0x01;
static const uint8_t OP_SHUTDOWN = 0x02;
static const uint8_t OP_HARD_OFF = 0x03;

// ---- configuration (the `blecfg` flash partition) ----

// Same scheme as pwrcfg/fancfg: a 4 KB partition written at flash time, so
// the secret never sits in the app image or the repo, survives reflashes,
// and needs no toolchain to (re)write. Layout, matching tools/blecfg.py:
//
//     "BLE1" magic, then
//     enabled(1) token(16) name(16)
//
// token is raw bytes, NUL-padded; name is ASCII, NUL-padded. Fields only
// ever get APPENDED (same compatibility rule as the other cfg partitions).

static const uint16_t WIRE_LEN = 1 + TOKEN_LEN + NAME_LEN;

struct Config
{
    bool enabled = false;
    uint8_t token[TOKEN_LEN] = {};
    char name[NAME_LEN + 1] = "BC250";

    bool decode(const uint8_t* p, uint16_t len)
    {
        if (len < WIRE_LEN)
            return false;

        enabled = p[0] != 0;
        memcpy(token, p + 1, TOKEN_LEN);
        memcpy(name, p + 1 + TOKEN_LEN, NAME_LEN);
        name[NAME_LEN] = 0;
        if (!name[0])
            strcpy(name, "BC250");
        return true;
    }
};

static Config g_cfg; // loaded once in start(), read-only after

static bool loadConfig(Config& c)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "blecfg");
    if (!part)
    {
        BLOG("no blecfg partition (older table?) — remote off");
        return false;
    }

    uint8_t b[4 + WIRE_LEN];
    if (esp_partition_read(part, 0, b, sizeof b) != ESP_OK)
        return false;

    if (memcmp(b, "BLE1", 4) != 0)
        return false;

    return c.decode(b + 4, WIRE_LEN);
}

// ---- state ----

// written by NimBLE host-task callbacks, read by the policy task; lock-free
// aligned reads of single values, same justification as pwr::senseState()
static volatile bool g_synced = false; // host/controller sync done, can advertise
static volatile uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t g_ownAddrType = 0;
static uint16_t g_statusHandle = 0; // status characteristic's value handle

// policy task locals
static int g_lastState = -2;   // last pwr::psuState() seen (-2 = never)
static uint16_t g_advItvl = 0; // interval the running advertisement was
                               // started with (0 = none), to restart it when
                               // the PSU state calls for the other pace

// ---- GATT ----

static int ctrlAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t buf[TOKEN_LEN + 1];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0 ||
        len != sizeof buf)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    // constant-time token compare — not that a BLE latency oracle is a real
    // worry, but it costs nothing
    uint8_t diff = 0;
    for (int i = 0; i < TOKEN_LEN; i++)
        diff |= buf[i] ^ g_cfg.token[i];
    if (diff)
    {
        BLOG("command with a wrong token rejected");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    uint8_t op = buf[TOKEN_LEN];
    if (op == OP_POWER_ON)
        pwr::remoteRequest(pwr::REMOTE_ON);
    else if (op == OP_SHUTDOWN)
        pwr::remoteRequest(pwr::REMOTE_OFF);
    else if (op == OP_HARD_OFF)
        pwr::remoteRequest(pwr::REMOTE_OFF_HARD);
    else
        return BLE_ATT_ERR_UNLIKELY;

    BLOG("command 0x%02x accepted", op);
    return 0;
}

static int statAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    int st = pwr::psuState();
    uint8_t b = st < 0 ? 0 : (uint8_t)st;
    return os_mbuf_append(ctxt->om, &b, 1) == 0 ? 0
                                                : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// built in start() with plain field assignment: NimBLE's struct layouts have
// grown fields across IDF versions, and C++ designated initializers would
// pin this file to one ordering
static ble_gatt_chr_def g_chrs[3];
static ble_gatt_svc_def g_svcs[2];

// ---- GAP / advertising ----

static int gapEvent(ble_gap_event* ev, void*)
{
    switch (ev->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0)
        {
            g_conn = ev->connect.conn_handle;
            BLOG("phone connected");
        }
        // a failed connect leaves us idle; the policy task re-advertises
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn = BLE_HS_CONN_HANDLE_NONE;
        BLOG("phone disconnected (reason=%d)", ev->disconnect.reason);
        break;

    default:
        break;
    }
    return 0;
}

static void startAdv(uint16_t itvl)
{
    // service UUID in the advertisement (Web Bluetooth filters on it), name
    // in the scan response — together they'd overflow the 31-byte adv PDU
    ble_hs_adv_fields f = {};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = const_cast<ble_uuid128_t*>(&SVC_UUID);
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;

    ble_hs_adv_fields rsp = {};
    rsp.name = (const uint8_t*)g_cfg.name;
    rsp.name_len = strlen(g_cfg.name);
    rsp.name_is_complete = 1;

    ble_gap_adv_params p = {};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = itvl;
    p.itvl_max = itvl;

    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0)
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc == 0)
        rc = ble_gap_adv_start(g_ownAddrType, nullptr, BLE_HS_FOREVER, &p,
                               gapEvent, nullptr);
    if (rc == 0)
        g_advItvl = itvl;
    else
        BLOG("adv start failed rc=%d", rc);
}

static void onSync()
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &g_ownAddrType);
    g_synced = true; // the policy task takes it from here
}

static void onReset(int reason) { BLOG("host reset, reason=%d", reason); }

static void hostTask(void*)
{
    nimble_port_run(); // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ---- the policy task ----

// owns the advertising pace: quick while the PSU is off, slow while it is on
// (see ADV_ITVL_*), paused while a phone is connected (one connection is the
// whole clientele), and notifies the status characteristic on state changes.
static void loop()
{
    int st = pwr::psuState(); // 0/1/2; start() refused to run on -1

    if (st != g_lastState)
    {
        g_lastState = st;
        if (g_statusHandle)
            ble_gatts_chr_updated(g_statusHandle); // notify subscribers
        BLOG("psu state -> %d", st);
    }

    bool connected = g_conn != BLE_HS_CONN_HANDLE_NONE;
    uint16_t itvl = st == 0 ? ADV_ITVL_OFF : ADV_ITVL_ON;

    // a pace change restarts the advertisement (stop is synchronous, so the
    // branch below starts it again at the new interval within this poll)
    if (ble_gap_adv_active() && g_advItvl != itvl)
        ble_gap_adv_stop();

    if (g_synced && !connected && !ble_gap_adv_active())
        startAdv(itvl);
    else if (connected && ble_gap_adv_active())
        ble_gap_adv_stop();
}

static void taskMain(void*)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        loop();
    }
}

// ---- public API ----

void start()
{
    if (!loadConfig(g_cfg) || !g_cfg.enabled)
        return; // not opted in: the stack is never initialized, no RAM spent

    if (pwr::psuState() < 0)
    {
        // remote for a power switch that isn't there — likely a blecfg left
        // over after PWR=off. Say so; silently doing nothing looks like a
        // radio fault from the phone's side.
        BLOG("power switch is off — remote disabled (flash PWR=on, or "
             "BLE=off to silence this)");
        return;
    }

    if (nimble_port_init() != ESP_OK)
    {
        BLOG("nimble_port_init FAILED — remote off");
        return;
    }

    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.reset_cb = onReset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(g_cfg.name);

    g_chrs[0] = {};
    g_chrs[0].uuid = &CTRL_UUID.u;
    g_chrs[0].access_cb = ctrlAccess;
    g_chrs[0].flags = BLE_GATT_CHR_F_WRITE;
    g_chrs[1] = {};
    g_chrs[1].uuid = &STAT_UUID.u;
    g_chrs[1].access_cb = statAccess;
    g_chrs[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    g_chrs[1].val_handle = &g_statusHandle;
    g_chrs[2] = {}; // terminator

    g_svcs[0] = {};
    g_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[0].uuid = &SVC_UUID.u;
    g_svcs[0].characteristics = g_chrs;
    g_svcs[1] = {}; // terminator

    if (ble_gatts_count_cfg(g_svcs) != 0 || ble_gatts_add_svcs(g_svcs) != 0)
    {
        BLOG("GATT registration FAILED — remote off");
        return;
    }

    nimble_port_freertos_init(hostTask);

    BLOG("remote up: \"%s\"", g_cfg.name);

    // same priority tier as the pwr task: a 250 ms policy poll never needs to
    // win against the strip's latch cadence
    xTaskCreate(taskMain, "ble_pwr", 4096, nullptr, 2, nullptr);
}
} // namespace ble
