#include "ble.hpp"

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

#include "cfgstore.hpp"
#include "dash.hpp"
#include "dbglog.hpp"
#include "fan.hpp"
#include "fanwire.hpp"
#include "hostreq.hpp"
#include "led_service.hpp"
#include "link.hpp"
#include "power_switch.hpp"
#include "protocol.hpp"
#include "util.hpp"

#define BLOG(fmt, ...) dbglog::line("ble: " fmt, ##__VA_ARGS__)

// The GATT surface — one custom service, eight characteristics:
//
//   control (write):        token(16) op(1) [args]. Token is the flash-time
//                           shared secret, byte-for-byte (short tokens
//                           NUL-padded — the web page pads the same way). op
//                           0x01 = power on, 0x02 = graceful shutdown, 0x03 =
//                           hard off (release PS_ON#: the remote form of
//                           holding the button, for a crashed machine) — no
//                           args. op 0x10 = set a fan slot's standalone
//                           record, args slot(1) (0-based) then the slot's
//                           record in CMD_FAN_STANDALONE's per-slot layout
//                           (protocol.hpp FAN_HEADER_LEN bytes): its
//                           output (a header of this board or a GPIO),
//                           resting duty, boost and boost length, ramp,
//                           and input — FAN_KIND_FALLBACK (the fan runs its
//                           fallback), FAN_KIND_GPIO with the input pin and
//                           the (input %, duty %) curve this board runs
//                           itself, FAN_KIND_RECEIVER_TEMP with a (°C,
//                           duty %) curve over this chip's own temperature,
//                           or FAN_KIND_HOST (the fallback until a
//                           daemon drives it); a record with no output
//                           removes the slot's fan. Applied and persisted by
//                           the fan module without any daemon — what makes
//                           this board a fan controller on its own. Rejected
//                           (VALUE_ERR, reason in the debug log) for a value
//                           out of range, a bad curve, or a pin this board
//                           can't drive or read on. op 0x20 =
//                           set the power switch's tunings, args the eight
//                           bytes of proto::CMD_PWR_TUNING (hold ms, boot
//                           timeout ms, sense low/high mV, LE u16s): applied
//                           and persisted by the power switch (pwr::
//                           setTuning), which rejects an out-of-range set
//                           whole (VALUE_ERR) — for a receiver with no
//                           daemon to route the edit through; a running
//                           daemon's config wins again at its next start.
//                           op 0x21 = set the power switch's wake input
//                           pin, args pin(1) (a GPIO, 0xFF = none), the
//                           byte of proto::CMD_PWR_WAKE: applied and
//                           persisted by the power switch (pwr::setWakePin),
//                           which refuses a pin this board can't read on
//                           (VALUE_ERR, reason in the debug log) — the info
//                           value lists the ones it can.
//                           A wrong token is rejected with "write not
//                           permitted" (TOKEN_ERR, below); a power op
//                           (0x01-0x03, 0x20, 0x21) on a receiver with no
//                           power switch with VALUE_ERR; a right power op
//                           stages the request with the pwr task and
//                           succeeds even if the state makes it moot (the
//                           status characteristic is how a client sees what
//                           actually happened).
//   status (read + notify): one byte, the host's coarse state: 0 = off,
//                           1 = booting, 2 = on — pwr's PSU state, or on a
//                           receiver with no power switch whether the
//                           daemon is streaming (hostState). Notifies on
//                           change, so the phone watches the power-on it
//                           asked for confirm itself via the sense wire.
//   fans (read + notify):   this board's own live view, FANS_LEN bytes
//                           (layout at buildFans): per slot the duty it
//                           applies and where it came from, the PSU state,
//                           how old the daemon's telemetry is. Notified once
//                           a second while subscribed.
//   fansa (read + notify):  this board's standalone fan settings as stored —
//                           CMD_FAN_STANDALONE's layout (protocol.hpp): one
//                           record per slot with its output, fallback, boost
//                           and boost length, ramp, input kind, and a gpio
//                           slot's pin and curve. What the page shows and
//                           edits with no daemon around (op 0x10 writes one
//                           slot's record); notified when it changes.
//   info (read):            build facts: firmware version string, free heap,
//                           the GPIOs a gpio:N fan input or the wake input
//                           may read on, the board's header map, and the
//                           GPIOs a gpio:N fan output may drive.
//   pwr (read + notify):    this board's power switch, PWR_LEN bytes (layout
//                           at buildPwr): the wiring, the tunings in force,
//                           the PSU state and what the sense wire reads right
//                           now — the calibration view. Notified once a
//                           second while subscribed, like fans.
//   views (write + notify): the daemon's views (protocol.hpp CMD_VIEW: the
//                           fan list, its telemetry, the sensor catalogue,
//                           the strip, the power switch — JSON text this
//                           board relays unread, dash.hpp). A notification
//                           is view(1) len(2, the whole view's, LE) then as
//                           much of the view as the MTU leaves room for,
//                           sent when a new one lands; one cut short (or any
//                           view on connecting) is read whole through the
//                           page characteristic. A write is token(16) view(1)
//                           then an edit (JSON text, the view owner's shape),
//                           relayed to the daemon as MSG_EDIT; the daemon's
//                           answering view is notified. A subscription is
//                           what tells the daemon a phone watches
//                           (MSG_WATCH), which starts its telemetry and
//                           catalogue; unsubscribing stops them.
//   page (read + write):    a daemon view, whole, in pages — a GATT value
//                           holds 512 bytes, a view up to protocol.hpp
//                           VIEW_MAX. A write of [view][page] (no token —
//                           the views are readable anyway) picks the page;
//                           page 0, or another view, takes a snapshot of the
//                           view, and every later page serves the same
//                           snapshot, so the pages of one read always belong
//                           together. A read is ver(1) = 1 view(1) page(1)
//                           pages(1) len(2, the whole view's, LE) then that
//                           page's PAGE_DATA bytes (fewer on the last).
//
// The 128-bit UUIDs are this project's own (random base, "bc250" spelled
// into the tail); the web page must list the service UUID to find us.
// NimBLE wants them little-endian:
//   service a5f20001-8f11-4e0e-9b3a-0bc250e0c001, control ...0002,
//   status ...0003, fans ...0004, info ...0006, fansa ...0009, pwr ...000B,
//   page ...000D, views ...000E. 0005, 0007, 0008, 000A and 000C were one
//   characteristic per daemon view (fancfg, telem, stripcfg, sensors,
//   pwrcfg), replaced by views; retired, never reused.
namespace ble
{
static const uint32_t POLL_MS = 250; // policy task cadence

// Advertising runs in both PSU states — a phone must be able to reach a
// machine that crashed, which is precisely when the host is "up" — at one
// quick pace: opening the page waits for an advertisement twice (the page's
// watch, then the connect), so the interval is most of the time to connect.
// 300 ms is gentle on the 5VSB budget, and the strip can't feel the radio:
// its bitstream leaves by DMA (render.cpp), and advertising pauses while a
// phone is connected anyway. (It was 1.28 s while on, back when the strip
// went out over RMT, whose interrupt-fed refills the radio could tear.)
static const uint16_t ADV_ITVL = 0x01E0; // 480 × 0.625 ms = 300 ms

// The connection interval asked of a phone once it connects. A connected pair
// only exchanges packets once per interval, and every GATT op the page makes
// (a subscribe, a read, one page of a long value) is a request plus an answer
// with one outstanding at a time — so each costs an interval or two, and
// opening the dashboard is ~30 of them. Phones pick 30–50 ms on their own;
// 15 ms makes the load ~3× quicker. It is the shortest Apple's accessory
// guidelines accept as a fixed interval (min = max = 15 ms), and Android takes
// it too; a phone that declines keeps its own, and nothing else changes. The
// cost is a radio event every 15 ms while a phone is connected — the strip is
// DMA-fed (render.cpp) and can't feel it.
static const uint16_t CONN_ITVL = 12;        // × 1.25 ms = 15 ms
static const uint16_t CONN_TIMEOUT = 400;    // × 10 ms = 4 s supervision timeout

// The advertisement also says what the host is doing, so the page's receiver
// list can show which boards are on without connecting to each: manufacturer
// data under 0xFFFF (the SIG's id for unregistered use) = ver(1) = 1,
// state(1) = hostState(). Flags (3) + the service UUID (18) + this (6) = 27
// of the 31-byte PDU.
static const uint16_t ADV_MFG_ID = 0xFFFF;
static const uint8_t ADV_MFG_VER = 1;

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

// what a well-formed command with an unusable value gets (the fan op naming a
// header that isn't wired): the first ATT application error code, distinct
// from the length and token refusals in a debug log
static const int VALUE_ERR = 0x80;

// the project's UUID base with the characteristic's number in the 13th byte
#define BC250_UUID(n)                                                                    \
    BLE_UUID128_INIT(0x01, 0xc0, 0xe0, 0x50, 0xc2, 0x0b, 0x3a, 0x9b, 0x0e, 0x4e, 0x11, 0x8f, \
                     (n), 0x00, 0xf2, 0xa5)
static const ble_uuid128_t SVC_UUID = BC250_UUID(0x01);
static const ble_uuid128_t CTRL_UUID = BC250_UUID(0x02);
static const ble_uuid128_t STAT_UUID = BC250_UUID(0x03);
static const ble_uuid128_t FANS_UUID = BC250_UUID(0x04);
static const ble_uuid128_t INFO_UUID = BC250_UUID(0x06);
static const ble_uuid128_t FANSA_UUID = BC250_UUID(0x09);
static const ble_uuid128_t PWR_UUID = BC250_UUID(0x0b);
static const ble_uuid128_t PAGE_UUID = BC250_UUID(0x0d);
static const ble_uuid128_t VIEWS_UUID = BC250_UUID(0x0e);

static const uint8_t OP_POWER_ON = 0x01;
static const uint8_t OP_SHUTDOWN = 0x02;
static const uint8_t OP_HARD_OFF = 0x03;
static const uint8_t OP_FAN_HEADER = 0x10; // + slot(1) + one slot's record (FAN_HEADER_LEN)
static const uint8_t OP_PWR_TUNING = 0x20; // + the CMD_PWR_TUNING payload (8)
static const uint8_t OP_PWR_WAKE = 0x21;   // + the CMD_PWR_WAKE payload (1)
static const uint16_t OP_FAN_HEADER_ARGS = 1 + proto::FAN_HEADER_LEN; // the longest args

// dashboard cadence: the fans and pwr values are notified this often while
// subscribed (the daemon's telemetry moves on its 0.5 s tick, the fan task's
// on 100 ms, the sense wire is sampled every poll; a phone doesn't need more
// than 1 Hz, and a few dozen bytes a second is nothing to the radio), the
// watch keepalive goes to the daemon this often, and telemetry older than
// this reads as "daemon gone"
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
    uint8_t b[4 + WIRE_LEN];
    bool found = false;
    if (!cfgstore::partition("blecfg", b, sizeof b, &found))
    {
        if (!found)
            BLOG("no blecfg partition (older table?) — remote off");
        return false;
    }

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

// the characteristics, one row each (filled into NimBLE's own table in
// start()): the value handle registration hands back, whether a client has
// its notifications on (written by the host-task GAP callback, read by the
// policy task), and — for a value that mirrors something with a change
// counter — the counter last notified, so the policy loop can be one pass
// over this table instead of a paragraph per characteristic
struct Chr
{
    const ble_uuid128_t* uuid = nullptr;
    ble_gatt_access_fn* access = nullptr;
    ble_gatt_chr_flags flags = 0;
    uint16_t handle = 0;
    volatile bool sub = false;
    uint32_t lastSeq = 0;
};
enum ChrId
{
    C_CTRL,
    C_STAT,
    C_FANS,
    C_INFO,
    C_FANSA,
    C_PWR,
    C_PAGE,
    C_VIEWS,
    C_COUNT
};
static Chr g_chr[C_COUNT];

static void defineChr(ChrId id, const ble_uuid128_t* uuid, ble_gatt_access_fn* access,
                      ble_gatt_chr_flags flags)
{
    g_chr[id].uuid = uuid;
    g_chr[id].access = access;
    g_chr[id].flags = flags;
}

// policy task locals
static int g_lastState = -2;   // last hostState() seen (-2 = never)
static int g_advState = -2;    // hostState() the running advertisement carries
static uint32_t g_lastPoll = 0;    // last 1 Hz notify of the fans and pwr values
static uint32_t g_lastWatch = 0;   // last MSG_WATCH 1 sent
static bool g_watching = false;    // the daemon has been told a phone watches
static uint32_t g_viewSeq[dash::VIEWS]; // each view's arrival count last notified

// the host's coarse state as the status value reports it: 0 off, 1
// booting, 2 on. The power switch's PSU state where there is one; without
// it (the remote runs for the fans and the strip alone) all this board can
// tell is whether the daemon is streaming, which reads as on or off.
static int hostState()
{
    int st = pwr::psuState();
    if (st >= 0)
        return st;
    return led::hostLive() ? 2 : 0;
}

// ---- the fans value ----

// FANS_LEN bytes, all little-endian — this board's side only; the daemon's
// numbers are in the telem value, and the page joins the two:
//   ver(1) = 3 (1 had no fallback byte per header, 2 no kind/in; the page
//            reads all three)
//   flags(1): bit0 fan feature on, bit1 boosting, bit2 hold (host powering
//             off), bit3 live (daemon duties in force), bit4 host present on
//             the link, bit5 telemetry fresh (younger than TELEM_FRESH_MS)
//   psu(1): 0 off, 1 booting, 2 on
//   telemAge(1): seconds since the daemon's last telemetry, 255 = none/stale
//   per slot ×6: state(1) duty(1) fallback(1) kind(1) in(1)
//     state: bit0 driving a pin, bits1-3 source (fan::SRC_*)
//     duty: the duty this board applies (0xFF: drives nothing)
//     fallback: the resting duty in force — the control op 0x10's record
//               sets it, so the page's slider shows what is stored (0xFF: drives nothing)
//     kind: the input kind in force (protocol.hpp FAN_KIND_*, 0xFF: drives nothing)
//     in: a gpio slot's sampled input percent, an esp32_temp slot's whole
//         °C (0xFF = no reading)
//   uptime(4): this board's seconds since reset
//   temp(2, signed): this chip's temperature in tenths of °C (fan::tempTenths),
//     -32768 = none (no sensor, fan feature off). A tail, not a new ver: a
//     ver-3 page reads the bytes before it and ignores the rest
static const uint16_t FANS_LEN = 4 + proto::FAN_CHANNELS * 5 + 4 + 2;

static const uint8_t F_ACTIVE = 0x01, F_BOOST = 0x02, F_HOLD = 0x04, F_LIVE = 0x08,
                     F_HOST = 0x10, F_TELEM = 0x20;

static uint16_t buildFans(uint8_t* p)
{
    fan::Snapshot s;
    fan::snapshot(s);

    uint32_t age = 0;
    dash::get(proto::VIEW_FAN_TELEM, nullptr, 0, nullptr, nullptr, &age);
    bool haveT = age != 0xFFFFFFFFu;
    bool fresh = haveT && age < TELEM_FRESH_MS;

    int st = hostState();
    uint16_t at = 0;
    p[at++] = 3;
    p[at++] = (s.active ? F_ACTIVE : 0) | (s.boosting ? F_BOOST : 0) |
              (s.hold ? F_HOLD : 0) | (s.live ? F_LIVE : 0) |
              (link::hostPresent() ? F_HOST : 0) | (fresh ? F_TELEM : 0);
    p[at++] = (uint8_t)st;
    p[at++] = !haveT || age >= 255000 ? 255 : (uint8_t)(age / 1000);

    for (int i = 0; i < proto::FAN_CHANNELS; i++)
    {
        p[at++] = (s.wired[i] ? 0x01 : 0) | ((s.source[i] & 7) << 1);
        p[at++] = s.duty[i];
        p[at++] = s.fallback[i];
        p[at++] = s.kind[i];
        p[at++] = s.in[i];
    }

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    for (int k = 0; k < 4; k++)
        p[at++] = (uint8_t)(up >> (8 * k));

    uint16_t t = (uint16_t)fan::tempTenths();
    p[at++] = (uint8_t)t;
    p[at++] = (uint8_t)(t >> 8);

    return at;
}

// ---- the pwr value ----

// PWR_LEN bytes, little-endian — this board's power switch (pwr::snapshot):
//   ver(1) = 1
//   flags(1): bit0 feature on, bit1 sense wire (ADC) in use
//   psu(1): 0 off, 1 booting, 2 on
//   senseMv(2): the last sense reading, 0xFFFF = none
//   holdMs(2) bootTimeoutMs(2) senseLowMv(2) senseHighMv(2): the tunings in
//     force (proto::CMD_PWR_TUNING's layout, so the page reuses one decoder)
//   pins(6): ps_on button button_gnd sense led wake, GPIO numbers, 0xFF unwired
static const uint16_t PWR_LEN = 3 + 2 + proto::PWR_TUNING_LEN + 6;

static const uint8_t P_ACTIVE = 0x01, P_SENSE = 0x02;

static uint16_t buildPwr(uint8_t* p)
{
    pwr::Snapshot s;
    pwr::snapshot(s);

    uint16_t at = 0;
    p[at++] = 1;
    p[at++] = (s.active ? P_ACTIVE : 0) | (s.sensePin >= 0 ? P_SENSE : 0);
    p[at++] = s.psu;
    p[at++] = (uint8_t)s.senseMv;
    p[at++] = (uint8_t)(s.senseMv >> 8);
    s.tuning.encode(p + at);
    at += proto::PWR_TUNING_LEN;
    const int8_t pins[6] = {s.psOnPin, s.buttonPin, s.buttonGndPin, s.sensePin, s.ledPin,
                            s.wakePin};
    for (int8_t g : pins)
        p[at++] = g < 0 ? 0xFF : (uint8_t)g;
    return at;
}

// ---- GATT ----

static int ctrlAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    // token, op, and the arguments (the fan header op's are the longest);
    // the length is checked per op below, once the token has been
    uint8_t buf[TOKEN_LEN + 1 + OP_FAN_HEADER_ARGS];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0 ||
        len < TOKEN_LEN + 1)
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
    uint16_t args = len - TOKEN_LEN - 1;

    // the power ops, on a receiver with no power switch: nothing to press
    // or tune — refused, rather than accepted and silently dropped
    bool powerOp = op == OP_POWER_ON || op == OP_SHUTDOWN || op == OP_HARD_OFF ||
                   op == OP_PWR_TUNING || op == OP_PWR_WAKE;
    if (powerOp && pwr::psuState() < 0)
    {
        BLOG("command 0x%02x refused — no power switch on this receiver", op);
        return VALUE_ERR;
    }

    if (op == OP_FAN_HEADER)
    {
        if (args != OP_FAN_HEADER_ARGS)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        const uint8_t* a = buf + TOKEN_LEN + 1;
        const char* why = nullptr;
        if (!fan::setHeader(a[0], a + 1, proto::FAN_HEADER_LEN, &why))
        {
            BLOG("fan slot %u record rejected — %s", a[0], why ? why : "?");
            return VALUE_ERR;
        }
        fanwire::Header h = fanwire::Header::decode(a + 1);
        if (!h.used())
            BLOG("fan slot %u cleared from the phone", a[0]);
        else
            BLOG("fan slot %u set from the phone: output %s%u, fallback %u%%, kind %u, gpio %u (%u points), ramp %u%%/s",
                 a[0], h.outKind == proto::FAN_OUT_HEADER ? "header" : "gpio:", h.out, h.fallback, h.kind,
                 h.gpio, h.npts, h.ramp);
        return 0;
    }

    if (op == OP_PWR_TUNING)
    {
        if (args != proto::PWR_TUNING_LEN)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        const char* why = nullptr;
        if (!pwr::setTuning(buf + TOKEN_LEN + 1, args, &why))
        {
            BLOG("power tuning rejected — %s", why ? why : "?");
            return VALUE_ERR;
        }
        BLOG("power tuning set from the phone");
        return 0;
    }

    if (op == OP_PWR_WAKE)
    {
        if (args != 1)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t pin = buf[TOKEN_LEN + 1];
        const char* why = nullptr;
        if (!pwr::setWakePin(pin, &why))
        {
            BLOG("wake pin gpio%u rejected — %s", pin, why ? why : "?");
            return VALUE_ERR;
        }
        if (pin == 0xFF)
            BLOG("wake input switched off from the phone");
        else
            BLOG("wake input set to gpio%u from the phone", pin);
        return 0;
    }

    if (args != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
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

    uint8_t b = (uint8_t)hostState();
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

// this board's own views, built fresh on every read (and on the notification
// NimBLE builds through the same path): always the state as of now
template <uint16_t (*BUILD)(uint8_t*), uint16_t MAX>
static int viewAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t b[MAX];
    uint16_t n = BUILD(b);
    return n == 0 || os_mbuf_append(ctxt->om, b, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// the stored standalone fan settings, in the view shape above
static uint16_t buildFansa(uint8_t* p) { return fan::standalone(p, proto::FAN_STANDALONE_LEN); }

// ---- the page characteristic (see the header comment) ----

static const uint16_t PAGE_DATA = 500; // a page's payload bytes: header + this <= 512
static uint8_t g_snap[dash::MAX_LEN];  // the snapshot the pages serve (host task only:
static uint16_t g_snapLen = 0;         // access callbacks and GAP events run there)
static int g_snapSlot = -1;            // the snapshot's view, -1 = none taken
static uint8_t g_page = 0;

static int pageAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        uint8_t a[2];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, a, sizeof a, &len) != 0 || len != 2)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (a[0] >= dash::VIEWS)
            return VALUE_ERR;
        // page 0 takes the snapshot; a later page of a view other than the
        // snapshot's is refused rather than served from a fresh one, which
        // would stitch two versions of the view together
        if (a[1] != 0 && a[0] != g_snapSlot)
            return VALUE_ERR;
        if (a[1] == 0)
        {
            g_snapLen = dash::get(a[0], g_snap, sizeof g_snap);
            g_snapSlot = a[0];
        }
        g_page = a[1];
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    // a read before any write: an empty value (no view picked)
    if (g_snapSlot < 0)
        return 0;
    uint16_t pages = g_snapLen ? (uint16_t)((g_snapLen + PAGE_DATA - 1) / PAGE_DATA) : 1;
    uint32_t from = (uint32_t)g_page * PAGE_DATA;
    uint16_t n = from >= g_snapLen ? 0 : (uint16_t)(g_snapLen - from < PAGE_DATA ? g_snapLen - from : PAGE_DATA);
    const uint8_t hdr[6] = {1, (uint8_t)g_snapSlot, g_page, (uint8_t)pages, (uint8_t)(g_snapLen & 0xFF),
                            (uint8_t)(g_snapLen >> 8)};
    if (os_mbuf_append(ctxt->om, hdr, sizeof hdr) != 0 ||
        (n && os_mbuf_append(ctxt->om, g_snap + from, n) != 0))
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    return 0;
}

// a phone's edit of a daemon view: token(16) view(1) + the edit (JSON text),
// relayed to the daemon unopened as MSG_EDIT — which view it is and whether
// the edit is any good are the daemon's business. Bounded by what one msg
// frame carries (hostreq::MSG_MAX); the page keeps its edits to one fan, the
// globals, or a few strip values, far under it.
static int viewsAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    // static, not stack: half a kilobyte, and only the NimBLE host task runs this
    static uint8_t buf[TOKEN_LEN + hostreq::MSG_MAX];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0 || len <= TOKEN_LEN + 2)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t view = buf[TOKEN_LEN];
    if (!tokenOk(buf))
    {
        BLOG("view %u edit with a wrong token rejected", (unsigned)view);
        return TOKEN_ERR;
    }

    if (!hostreq::post(proto::MSG_EDIT, buf + TOKEN_LEN, len - TOKEN_LEN))
    {
        BLOG("view %u edit dropped — message queue full", (unsigned)view);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    BLOG("view %u edit (%u bytes) relayed to the daemon", (unsigned)view, (unsigned)(len - TOKEN_LEN - 1));
    return 0;
}

// ver(1) = 3, version(32, NUL-padded; the app image's PROJECT_VER — the git
// describe of the build), freeHeap(4), minFreeHeap(4), then since ver 2
// inputPins(8): the GPIOs a gpio:N fan input or the wake input may read on
// (fan::inputPins), little-endian, bit N = GPIO N; since ver 3 headerPins(6):
// the board's header map, header n's GPIO at [n-1], 0xFF = no such header
// (fan::headerPins), and outputPins(8): the GPIOs a gpio:N fan output may
// drive (fan::outputPins), a mask like inputPins. A ver-3 page is also one
// that pages (the page characteristic, the 25-byte fan record and its
// two-byte msg frames came with it). The page re-reads this after the wake
// pin or a fan's output moves, the things that change it while a phone is
// connected.
static int infoAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t b[1 + 32 + 4 + 4 + 8 + proto::FAN_CHANNELS + 8] = {};
    b[0] = 3;
    const esp_app_desc_t* d = esp_app_get_description();
    strncpy((char*)b + 1, d->version, 31);
    uint32_t heap = esp_get_free_heap_size();
    uint32_t minHeap = esp_get_minimum_free_heap_size();
    uint64_t pins = fan::inputPins();
    for (int k = 0; k < 4; k++)
    {
        b[33 + k] = (uint8_t)(heap >> (8 * k));
        b[37 + k] = (uint8_t)(minHeap >> (8 * k));
    }
    for (int k = 0; k < 8; k++)
        b[41 + k] = (uint8_t)(pins >> (8 * k));
    fan::headerPins(b + 49);
    uint64_t outs = fan::outputPins();
    for (int k = 0; k < 8; k++)
        b[49 + proto::FAN_CHANNELS + k] = (uint8_t)(outs >> (8 * k));
    return os_mbuf_append(ctxt->om, b, sizeof b) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// NimBLE's own tables, filled from g_chr in start() with plain field
// assignment: NimBLE's struct layouts have grown fields across IDF versions,
// and C++ designated initializers would pin this file to one ordering
static ble_gatt_chr_def g_chrs[C_COUNT + 1];
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
            ble_gap_upd_params up = {};
            up.itvl_min = CONN_ITVL;
            up.itvl_max = CONN_ITVL;
            up.latency = 0;
            up.supervision_timeout = CONN_TIMEOUT;
            int rc = ble_gap_update_params(g_conn, &up);
            if (rc != 0)
                BLOG("connection interval request failed rc=%d", rc);
        }
        // a failed connect leaves us idle; the policy task re-advertises
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn = BLE_HS_CONN_HANDLE_NONE;
        g_snapSlot = -1; // the next phone takes its own snapshot
        // the policy task tells the daemon nobody is watching any more
        for (Chr& c : g_chr)
            c.sub = false;
        BLOG("phone disconnected (reason=%d)", ev->disconnect.reason);
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
    {
        // what the phone settled on (asked or not), for the debug log
        ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0)
            BLOG("connection interval %u.%02u ms (status %d)", (unsigned)(d.conn_itvl * 125 / 100),
                 (unsigned)(d.conn_itvl * 125 % 100), ev->conn_update.status);
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        // the phone opened (or closed) the dashboard: notifications on the
        // views are what start the daemon's telemetry flowing
        for (Chr& c : g_chr)
            if (c.handle && ev->subscribe.attr_handle == c.handle)
                c.sub = ev->subscribe.cur_notify != 0;
        break;

    default:
        break;
    }
    return 0;
}

static void startAdv(int st)
{
    // service UUID and host state in the advertisement (Web Bluetooth
    // filters on the UUID), name in the scan response — together they'd
    // overflow the 31-byte adv PDU
    ble_hs_adv_fields f = {};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = const_cast<ble_uuid128_t*>(&SVC_UUID);
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    const uint8_t mfg[4] = {(uint8_t)(ADV_MFG_ID & 0xFF), (uint8_t)(ADV_MFG_ID >> 8), ADV_MFG_VER, (uint8_t)st};
    f.mfg_data = mfg;
    f.mfg_data_len = sizeof mfg;

    ble_hs_adv_fields rsp = {};
    rsp.name = (const uint8_t*)g_cfg.name;
    rsp.name_len = strlen(g_cfg.name);
    rsp.name_is_complete = 1;

    ble_gap_adv_params p = {};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = ADV_ITVL;
    p.itvl_max = ADV_ITVL;

    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0)
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc == 0)
        rc = ble_gap_adv_start(g_ownAddrType, nullptr, BLE_HS_FOREVER, &p,
                               gapEvent, nullptr);
    if (rc == 0)
        g_advState = st;
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

// notify a value whose source counts its changes, when the count moved
static void notifyOnChange(Chr& c, uint32_t seq)
{
    if (seq == c.lastSeq)
        return;
    c.lastSeq = seq;
    if (c.sub)
        ble_gatts_chr_updated(c.handle);
}

// notify each daemon view that changed since the last pass (the views
// characteristic's layout, in the header comment). A view that can't go out
// now — no buffer for it in NimBLE's pool — keeps its count and goes on a
// later pass; with nobody subscribed the counts just catch up.
static void notifyViews()
{
    // policy task only: one notification's worth, the ATT maximum
    static uint8_t b[512];
    uint16_t conn = g_conn;
    bool on = g_chr[C_VIEWS].sub && conn != BLE_HS_CONN_HANDLE_NONE;
    uint16_t mtu = on ? ble_att_mtu(conn) : 0;
    uint16_t room = mtu > 3 ? mtu - 3 : 0; // a notification's payload: the MTU less its own header
    if (room > sizeof b)
        room = sizeof b;

    for (uint8_t v = 0; v < dash::VIEWS; v++)
    {
        uint32_t seq = 0;
        uint16_t len = 0;
        if (!on || room <= 3)
        {
            dash::get(v, nullptr, 0, nullptr, &seq);
            g_viewSeq[v] = seq;
            continue;
        }
        uint16_t n = dash::get(v, b + 3, room - 3, &len, &seq);
        if (seq == g_viewSeq[v])
            continue;
        b[0] = v;
        b[1] = (uint8_t)len;
        b[2] = (uint8_t)(len >> 8);
        os_mbuf* om = ble_hs_mbuf_from_flat(b, 3 + n);
        if (!om || ble_gatts_notify_custom(conn, g_chr[C_VIEWS].handle, om) != 0)
            return; // out of buffers (notify_custom frees om either way): next pass
        g_viewSeq[v] = seq;
    }
}

// the dashboard's half of the policy. While a phone has the views
// subscribed, keep the daemon's telemetry alive with a watch keepalive and
// notify each new view; while it has the fans or pwr value subscribed, notify
// it once a second (NimBLE rebuilds the value through its access callback);
// notify a stored setting when it changes. With nobody subscribed none of
// this runs — the daemon then reads and sends nothing for the dashboard
// either.
static void dashboard(uint32_t now)
{
    if (g_chr[C_VIEWS].sub)
    {
        if (!g_watching || now - g_lastWatch >= WATCH_MS)
        {
            uint8_t on = 1;
            if (hostreq::post(proto::MSG_WATCH, &on, 1))
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
        hostreq::post(proto::MSG_WATCH, &off, 1);
    }

    if (now - g_lastPoll >= FANS_POLL_MS)
    {
        g_lastPoll = now;
        for (ChrId id : {C_FANS, C_PWR})
            if (g_chr[id].sub)
                ble_gatts_chr_updated(g_chr[id].handle);
    }

    notifyViews();

    uint32_t seq = 0;
    fan::standalone(nullptr, 0, &seq);
    notifyOnChange(g_chr[C_FANSA], seq);
    notifyOnChange(g_chr[C_PWR], pwr::settingsSeq());
}

// owns the advertising: the host state in it kept current, paused while a
// phone is connected (one connection is the whole clientele), and notifies
// the status characteristic on state changes.
static void loop()
{
    int st = hostState();

    if (st != g_lastState)
    {
        g_lastState = st;
        if (g_chr[C_STAT].handle)
            ble_gatts_chr_updated(g_chr[C_STAT].handle); // notify subscribers
        BLOG("psu state -> %d", st);
    }

    dashboard(millis());

    bool connected = g_conn != BLE_HS_CONN_HANDLE_NONE;

    // a state change restarts the advertisement (stop is synchronous, so the
    // branch below starts it again carrying the new state within this poll)
    if (ble_gap_adv_active() && g_advState != st)
        ble_gap_adv_stop();

    if (g_synced && !connected && !ble_gap_adv_active())
        startAdv(st);
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

    // no power switch is fine: the fans and the strip are reached the same
    // way, the power ops are refused (ctrlAccess), and the page hides what
    // the pwr value says isn't there
    if (pwr::psuState() < 0)
        BLOG("no power switch on this receiver — remote without power control");

    if (nimble_port_init() != ESP_OK)
    {
        BLOG("nimble_port_init FAILED — remote off");
        return;
    }

    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.reset_cb = onReset;

    // the MTU we answer a phone's exchange with: the daemon's views are JSON
    // of up to a few hundred bytes, and a notification carries what the MTU
    // leaves room for (the page reads the rest through the page
    // characteristic); ask for the ATT maximum so a client that exchanges
    // gets most views whole in one packet
    ble_att_set_preferred_mtu(512);

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(g_cfg.name);

    const ble_gatt_chr_flags R = BLE_GATT_CHR_F_READ, W = BLE_GATT_CHR_F_WRITE,
                             N = BLE_GATT_CHR_F_NOTIFY;
    defineChr(C_CTRL, &CTRL_UUID, ctrlAccess, W);
    defineChr(C_STAT, &STAT_UUID, statAccess, R | N);
    defineChr(C_FANS, &FANS_UUID, viewAccess<buildFans, FANS_LEN>, R | N);
    defineChr(C_INFO, &INFO_UUID, infoAccess, R);
    defineChr(C_FANSA, &FANSA_UUID, viewAccess<buildFansa, proto::FAN_STANDALONE_LEN>, R | N);
    defineChr(C_PWR, &PWR_UUID, viewAccess<buildPwr, PWR_LEN>, R | N);
    defineChr(C_PAGE, &PAGE_UUID, pageAccess, R | W);
    defineChr(C_VIEWS, &VIEWS_UUID, viewsAccess, W | N);

    for (int i = 0; i < C_COUNT; i++)
    {
        g_chrs[i] = {};
        g_chrs[i].uuid = &g_chr[i].uuid->u;
        g_chrs[i].access_cb = g_chr[i].access;
        g_chrs[i].flags = g_chr[i].flags;
        g_chrs[i].val_handle = &g_chr[i].handle;
    }
    g_chrs[C_COUNT] = {}; // terminator

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
