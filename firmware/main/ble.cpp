#include "ble.hpp"

#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_partition.h"

#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_app_desc.h"
#include "esp_system.h"

#include "dash.hpp"
#include "dbglog.hpp"
#include "fan.hpp"
#include "hostreq.hpp"
#include "link.hpp"
#include "power_switch.hpp"
#include "protocol.hpp"
#include "util.hpp"

#define BLOG(fmt, ...) dbglog::line("ble: " fmt, ##__VA_ARGS__)

// The GATT surface — one custom service, eight characteristics:
//
//   control (write):        token(16) op(1). Token is the flash-time shared
//                           secret, byte-for-byte (short tokens NUL-padded —
//                           the web page pads the same way). op 0x01 = power
//                           on, 0x02 = graceful shutdown, 0x03 = hard off
//                           (release PS_ON#: the remote form of holding the
//                           button, for a crashed machine). A wrong token is
//                           rejected with "write not permitted" (TOKEN_ERR,
//                           below); a right one stages the request with the
//                           pwr task and succeeds even if the state makes it moot
//                           (the status characteristic is how a client sees
//                           what actually happened).
//   status (read + notify): one byte, pwr's coarse PSU state: 0 = off,
//                           1 = booting, 2 = on. Notifies on change, so the
//                           phone watches the power-on it asked for confirm
//                           itself via the sense wire.
//   fans (read + notify):   this board's own live view, FANS_LEN bytes
//                           (layout at buildFans): per header the duty it
//                           applies and where it came from, the PSU state,
//                           how old the daemon's telemetry is. Notified once
//                           a second while subscribed.
//   telem (read + notify):  the daemon's telemetry — the last CMD_FAN_TELEM
//                           verbatim (JSON text; empty until one arrives).
//                           Notified when a new one lands. A subscription is
//                           what tells the daemon to start sending them
//                           (MSG_FAN_WATCH); unsubscribing stops them.
//   fancfg (read + write + notify):
//                           the fan config as the daemon runs it — the last
//                           CMD_FAN_CONFIG verbatim (JSON text; empty until
//                           one arrives). A write is token(16) followed by a
//                           partial edit (JSON text, the daemon's shape),
//                           forwarded to the daemon as MSG_FAN_CONFIG; the
//                           daemon's answering CMD_FAN_CONFIG notifies the
//                           new value.
//   stripcfg (read + write + notify):
//                           the same for the strip: the last CMD_STRIP_CONFIG
//                           (brightness, gamma, white balance, the file: rule
//                           "scenes"); a write is token(16) + a partial edit
//                           relayed as MSG_STRIP_CONFIG, answered by the
//                           daemon's next CMD_STRIP_CONFIG.
//   info (read):            build facts: firmware version string, free heap.
//
// The 128-bit UUIDs are this project's own (random base, "bc250" spelled
// into the tail); the web page must list the service UUID to find us.
// NimBLE wants them little-endian:
//   service a5f20001-8f11-4e0e-9b3a-0bc250e0c001, control ...0002,
//   status ...0003, fans ...0004, fancfg ...0005, info ...0006, telem ...0007,
//   stripcfg ...0008
namespace ble
{
static const uint32_t POLL_MS = 250; // policy task cadence

// Advertising runs in both PSU states — a phone must be able to reach a
// machine that crashed, which is precisely when the host is "up" — but at
// two paces: quick to find while the machine is off (the everyday power-on
// case, and 300 ms is still gentle on the 5VSB budget), slow while it is on,
// where reaching us is the rare rescue case and the radio should stay a
// rounding error next to the strip's latch cadence.
static const uint16_t ADV_ITVL_OFF = 0x01E0; // 480 × 0.625 ms = 300 ms
static const uint16_t ADV_ITVL_ON = 0x0800;  // 2048 × 0.625 ms = 1.28 s

static const uint16_t TOKEN_LEN = 16;
static const uint16_t NAME_LEN = 16;

// the ATT error a wrong token gets. "Insufficient authentication" (what this
// returned until Sep 2026) is the ATT code for "encrypt/pair the link first",
// which is not what a bad application-level secret means — and on Android
// Chrome reports it, like nearly every other ATT error, to the web page as one
// opaque "GATT Error Unknown.", so the page could not tell a bad token from
// anything else. "Write not permitted" is one of the few codes every platform
// passes through distinctly ("GATT operation not permitted."); the page keys
// on it. The other errors below (bad length, unknown op, queue full) still
// reach an Android page as "Unknown"; the page words that honestly.
static const int TOKEN_ERR = BLE_ATT_ERR_WRITE_NOT_PERMITTED;

static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x01, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t CTRL_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x02, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t STAT_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x03, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t FANS_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x04, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t FANCFG_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x05, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t INFO_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x06, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t TELEM_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x07, 0x00, 0xf2, 0xa5);
static const ble_uuid128_t STRIPCFG_UUID = BLE_UUID128_INIT(
    0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b,
    0x0e, 0x4e, 0x11, 0x8f, 0x08, 0x00, 0xf2, 0xa5);

static const uint8_t OP_POWER_ON = 0x01;
static const uint8_t OP_SHUTDOWN = 0x02;
static const uint8_t OP_HARD_OFF = 0x03;

// dashboard cadence: the fans value is notified this often while subscribed
// (the daemon's telemetry moves on its 0.5 s tick, the fan task's on 100 ms;
// a phone doesn't need more than 1 Hz, and 36 bytes a second is nothing to
// the radio), the watch keepalive goes to the daemon this often, and
// telemetry older than this reads as "daemon gone"
static const uint32_t FANS_POLL_MS = 1000;
static const uint32_t WATCH_MS = 10000;
static const uint32_t TELEM_FRESH_MS = 15000;

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
static uint16_t g_statusHandle = 0; // value handles, filled by registration
static uint16_t g_fansHandle = 0;
static uint16_t g_fancfgHandle = 0;
static uint16_t g_infoHandle = 0;
static uint16_t g_telemHandle = 0;
static uint16_t g_stripcfgHandle = 0;
static volatile bool g_fansSub = false;   // a client has fans notifications on
static volatile bool g_fancfgSub = false; // ...fancfg's
static volatile bool g_telemSub = false;  // ...telem's (= the daemon should send)
static volatile bool g_stripcfgSub = false; // ...stripcfg's

// policy task locals
static int g_lastState = -2;   // last pwr::psuState() seen (-2 = never)
static uint16_t g_advItvl = 0; // interval the running advertisement was
                               // started with (0 = none), to restart it when
                               // the PSU state calls for the other pace
static uint32_t g_lastFansPoll = 0;
static uint32_t g_lastWatch = 0;   // last MSG_FAN_WATCH 1 sent
static bool g_watching = false;    // the daemon has been told a phone watches
static uint32_t g_lastCfgSeq = 0;  // dash FAN_CONFIG seq last notified
static uint32_t g_lastTelSeq = 0;  // dash FAN_TELEM seq last notified
static uint32_t g_lastStripSeq = 0; // dash STRIP_CONFIG seq last notified

// ---- the fans value ----

// FANS_LEN bytes, all little-endian — this board's side only; the daemon's
// numbers are in the telem value, and the page joins the two:
//   ver(1) = 1
//   flags(1): bit0 fan feature on, bit1 boosting, bit2 hold (host powering
//             off), bit3 live (daemon duties in force), bit4 host present on
//             the link, bit5 telemetry fresh (younger than TELEM_FRESH_MS)
//   psu(1): 0 off, 1 booting, 2 on
//   telemAge(1): seconds since the daemon's last telemetry, 255 = none/stale
//   per header ×6: state(1) duty(1)
//     state: bit0 wired, bits1-2 source (fan::SRC_*)
//     duty: the duty this board applies (0xFF unwired)
//   uptime(4): this board's seconds since reset
static const uint16_t FANS_LEN = 4 + proto::FAN_CHANNELS * 2 + 4;

static const uint8_t F_ACTIVE = 0x01, F_BOOST = 0x02, F_HOLD = 0x04, F_LIVE = 0x08,
                     F_HOST = 0x10, F_TELEM = 0x20;

static uint16_t buildFans(uint8_t* p)
{
    fan::Snapshot s;
    fan::snapshot(s);

    uint32_t age = 0;
    dash::get(dash::FAN_TELEM, nullptr, 0, nullptr, &age);
    bool haveT = age != 0xFFFFFFFFu;
    bool fresh = haveT && age < TELEM_FRESH_MS;

    int st = pwr::psuState();
    uint16_t at = 0;
    p[at++] = 1;
    p[at++] = (s.active ? F_ACTIVE : 0) | (s.boosting ? F_BOOST : 0) |
              (s.hold ? F_HOLD : 0) | (s.live ? F_LIVE : 0) |
              (link::hostPresent() ? F_HOST : 0) | (fresh ? F_TELEM : 0);
    p[at++] = st < 0 ? 0 : (uint8_t)st;
    p[at++] = !haveT || age >= 255000 ? 255 : (uint8_t)(age / 1000);

    for (int i = 0; i < proto::FAN_CHANNELS; i++)
    {
        p[at++] = (s.wired[i] ? 0x01 : 0) | ((s.source[i] & 3) << 1);
        p[at++] = s.duty[i];
    }

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    for (int k = 0; k < 4; k++)
        p[at++] = (uint8_t)(up >> (8 * k));

    return at;
}

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
        return TOKEN_ERR;
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

// the same token check the control write does; true when it matches
static bool tokenOk(const uint8_t* tok)
{
    uint8_t diff = 0;
    for (int i = 0; i < TOKEN_LEN; i++)
        diff |= tok[i] ^ g_cfg.token[i];
    return diff == 0;
}

static int fansAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    // a read (or the notification NimBLE builds through this same path) is
    // always the state as of now
    uint8_t b[FANS_LEN];
    uint16_t n = buildFans(b);
    return os_mbuf_append(ctxt->om, b, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// the daemon's payloads, served verbatim (dash.hpp); none yet reads as an
// empty value, which the page shows as "nothing from the daemon" rather than
// as broken
static int serve(ble_gatt_access_ctxt* ctxt, dash::Slot slot)
{
    uint8_t b[dash::MAX_LEN];
    uint16_t n = dash::get(slot, b, sizeof b);
    return n == 0 || os_mbuf_append(ctxt->om, b, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int telemAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    return serve(ctxt, dash::FAN_TELEM);
}

// a phone's edit: token(16) + a partial edit (JSON text), relayed to the
// daemon unopened as msg `kind` — validation is its job. Bounded by what one
// msg frame carries (hostreq::MSG_MAX); the page keeps its edits to one
// header, the globals, or a few strip values, far under it.
static int relayEdit(ble_gatt_access_ctxt* ctxt, uint8_t kind, const char* what)
{
    uint8_t buf[TOKEN_LEN + hostreq::MSG_MAX];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0 || len <= TOKEN_LEN + 2)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    if (!tokenOk(buf))
    {
        BLOG("%s edit with a wrong token rejected", what);
        return TOKEN_ERR;
    }

    if (!hostreq::post(kind, buf + TOKEN_LEN, len - TOKEN_LEN))
    {
        BLOG("%s edit dropped — message queue full", what);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    BLOG("%s edit (%u bytes) relayed to the daemon", what, (unsigned)(len - TOKEN_LEN));
    return 0;
}

static int fancfgAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return serve(ctxt, dash::FAN_CONFIG);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    return relayEdit(ctxt, proto::MSG_FAN_CONFIG, "fan");
}

static int stripcfgAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return serve(ctxt, dash::STRIP_CONFIG);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    return relayEdit(ctxt, proto::MSG_STRIP_CONFIG, "strip");
}

// ver(1) = 1, version(32, NUL-padded; the app image's PROJECT_VER — the git
// describe of the build), freeHeap(4), minFreeHeap(4)
static int infoAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t b[1 + 32 + 4 + 4] = {};
    b[0] = 1;
    const esp_app_desc_t* d = esp_app_get_description();
    strncpy((char*)b + 1, d->version, 31);
    uint32_t heap = esp_get_free_heap_size();
    uint32_t minHeap = esp_get_minimum_free_heap_size();
    for (int k = 0; k < 4; k++)
    {
        b[33 + k] = (uint8_t)(heap >> (8 * k));
        b[37 + k] = (uint8_t)(minHeap >> (8 * k));
    }
    return os_mbuf_append(ctxt->om, b, sizeof b) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// built in start() with plain field assignment: NimBLE's struct layouts have
// grown fields across IDF versions, and C++ designated initializers would
// pin this file to one ordering
static ble_gatt_chr_def g_chrs[8];
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
        // the policy task tells the daemon nobody is watching any more
        g_fansSub = false;
        g_fancfgSub = false;
        g_telemSub = false;
        g_stripcfgSub = false;
        BLOG("phone disconnected (reason=%d)", ev->disconnect.reason);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // the phone opened (or closed) the dashboard: notifications on the
        // telem value are what start the daemon's telemetry flowing
        if (ev->subscribe.attr_handle == g_fansHandle)
            g_fansSub = ev->subscribe.cur_notify != 0;
        else if (ev->subscribe.attr_handle == g_fancfgHandle)
            g_fancfgSub = ev->subscribe.cur_notify != 0;
        else if (ev->subscribe.attr_handle == g_telemHandle)
            g_telemSub = ev->subscribe.cur_notify != 0;
        else if (ev->subscribe.attr_handle == g_stripcfgHandle)
            g_stripcfgSub = ev->subscribe.cur_notify != 0;
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

// the dashboard's half of the policy. While a phone has the telem value
// subscribed, keep the daemon's telemetry alive with a watch keepalive and
// notify each new one; while it has the fans value subscribed, notify it once
// a second (NimBLE rebuilds the value through fansAccess); notify a new config
// when the daemon pushes one. With nobody subscribed none of this runs — the
// daemon then reads and sends nothing for the dashboard either.
static void dashboard(uint32_t now)
{
    if (g_telemSub)
    {
        if (!g_watching || now - g_lastWatch >= WATCH_MS)
        {
            uint8_t on = 1;
            if (hostreq::post(proto::MSG_FAN_WATCH, &on, 1))
            {
                g_watching = true;
                g_lastWatch = now;
            }
        }
    }
    else if (g_watching)
    {
        // the last phone left (unsubscribed or dropped): stop the telemetry
        g_watching = false;
        uint8_t off = 0;
        hostreq::post(proto::MSG_FAN_WATCH, &off, 1);
    }

    if (g_fansSub && now - g_lastFansPoll >= FANS_POLL_MS)
    {
        g_lastFansPoll = now;
        ble_gatts_chr_updated(g_fansHandle);
    }

    uint32_t seq = 0;
    dash::get(dash::FAN_TELEM, nullptr, 0, &seq);
    if (seq != g_lastTelSeq)
    {
        g_lastTelSeq = seq;
        if (g_telemSub)
            ble_gatts_chr_updated(g_telemHandle);
    }

    dash::get(dash::FAN_CONFIG, nullptr, 0, &seq);
    if (seq != g_lastCfgSeq)
    {
        g_lastCfgSeq = seq;
        if (g_fancfgSub)
            ble_gatts_chr_updated(g_fancfgHandle);
    }

    dash::get(dash::STRIP_CONFIG, nullptr, 0, &seq);
    if (seq != g_lastStripSeq)
    {
        g_lastStripSeq = seq;
        if (g_stripcfgSub)
            ble_gatts_chr_updated(g_stripcfgHandle);
    }
}

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

    dashboard(millis());

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

    // the MTU we answer a phone's exchange with: the daemon's config and
    // telemetry are JSON of up to a few hundred bytes, and a notification is
    // truncated to the MTU (a read is not — the page re-reads on a short
    // notification); ask for the ATT maximum so a client that exchanges
    // gets whole values in one packet
    ble_att_set_preferred_mtu(512);

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
    g_chrs[2] = {};
    g_chrs[2].uuid = &FANS_UUID.u;
    g_chrs[2].access_cb = fansAccess;
    g_chrs[2].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    g_chrs[2].val_handle = &g_fansHandle;
    g_chrs[3] = {};
    g_chrs[3].uuid = &FANCFG_UUID.u;
    g_chrs[3].access_cb = fancfgAccess;
    g_chrs[3].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY;
    g_chrs[3].val_handle = &g_fancfgHandle;
    g_chrs[4] = {};
    g_chrs[4].uuid = &INFO_UUID.u;
    g_chrs[4].access_cb = infoAccess;
    g_chrs[4].flags = BLE_GATT_CHR_F_READ;
    g_chrs[4].val_handle = &g_infoHandle;
    g_chrs[5] = {};
    g_chrs[5].uuid = &TELEM_UUID.u;
    g_chrs[5].access_cb = telemAccess;
    g_chrs[5].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
    g_chrs[5].val_handle = &g_telemHandle;
    g_chrs[6] = {};
    g_chrs[6].uuid = &STRIPCFG_UUID.u;
    g_chrs[6].access_cb = stripcfgAccess;
    g_chrs[6].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY;
    g_chrs[6].val_handle = &g_stripcfgHandle;
    g_chrs[7] = {}; // terminator

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
    // win against the strip's latch cadence. Stack sized for the dashboard's
    // value assembly (a few hundred bytes of buffers) on top of NimBLE's calls.
    xTaskCreate(taskMain, "ble_pwr", 5120, nullptr, 2, nullptr);
}
} // namespace ble
