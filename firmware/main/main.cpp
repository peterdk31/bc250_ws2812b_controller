// ESP-IDF receiver firmware (native — no Arduino, no arduino-cli). Replays
// pixel frames streamed from the host daemon over the serial link.
//
// The receiver renders no effects: the daemon records the power-on/shutdown
// animations to already-corrected pixel frames and streams them here
// (CMD_REC_*); this firmware stores them on LittleFS and replays them. So an
// effect tweak no longer means a reflash — it's picked up on the next daemon
// start and shown one power cycle later. The only device-specific parts are the
// four ports below (WS2812 output, NVS, LittleFS, the serial link) and millis();
// everything else — the wire protocol, framing, recording format and replay,
// and crossfading — is the shared common/ code the daemon also compiles.
//
// Ports from the old Arduino sketch:
//   Freenove_WS2812_Lib_for_ESP32  ->  espressif/led_strip (RMT)
//   Preferences                    ->  nvs_flash / nvs
//   LittleFS                       ->  joltwallet/littlefs (esp_vfs_littlefs)
//   Serial (UART0 / USB CDC)       ->  driver/uart (ESP32) | usb_serial_jtag (C3)
//   millis()                       ->  esp_timer_get_time()/1000
//   setup()/loop()                 ->  app_main() { setup(); for(;;) loop(); }

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_littlefs.h"
#include "led_strip.h"
#include "nvs.h"
#include "nvs_flash.h"

// Serial link: the plain ESP32 reaches the host over UART0 (wired to the USB
// bridge); native-USB chips (C3/C6/H2/S3) expose USB Serial/JTAG instead. The
// two use different drivers, selected here and wrapped by namespace `link`.
#if CONFIG_IDF_TARGET_ESP32
#define LINK_UART 1
#include "driver/uart.h"
#else
#define LINK_USB_SERIAL_JTAG 1
#include "driver/usb_serial_jtag.h"
#endif

// shared with the host (compiled straight from ../../common, on the include
// path): the wire protocol, the streaming frame parser, and the recording
// format + replay logic.
#include "protocol.hpp"
#include "receiver.hpp"
#include "recording.hpp"
#include "fade.hpp"
#include "dither.hpp"

#define MAX_LEDS 2048

// how often the strip is re-latched between host frames. Frames arrive as 8.8
// fixed-point values (see common/protocol.hpp); every latch rounds them to
// the strip's 8 bits with a fresh ordered threshold (common/dither.hpp), so a
// sub-code value renders as a duty cycle between adjacent codes. At the
// ~300-500 Hz this period yields (the serial read's ~2 ms block and the RMT
// transfer time set the real cadence) the toggling sits far above flicker
// fusion — the eye sees only the average, so dim gradients glide instead of
// stepping. A long strip's RMT time (~30 µs/LED) throttles this naturally;
// that's fine, the rate only needs to stay comfortably above ~200 Hz.
#define DITHER_REFRESH_US 2000

// a latch that comes this long after the previous one is too slow to dither
// invisibly (the duty cycle would read as flicker, not an average) — a very
// long strip's RMT transfer, or a starved loop. Such latches round to nearest
// instead: banding comes back, flicker doesn't. Kicks in below ~200 Hz.
#define SLOW_LATCH_US 5000

// LittleFS mount point and the recording files (one per slot; see
// common/protocol.hpp).
#define LFS_BASE "/lfs"
#define REC_PATH_POWER_ON LFS_BASE "/boot.rec"
#define REC_PATH_SHUTDOWN LFS_BASE "/shutdown.rec"

// TEMP diagnostic: uncomment to bypass the whole host/recording path and drive
// the strip directly from setup(). Proves the board + led_strip + our
// initStrip()/refresh actually light the strip on this pin, independent of the
// daemon link. Set the pin/count to your wiring. Recomment and reflash once
// confirmed. See the LED_SELFTEST block in setup().
// #define LED_SELFTEST
#define LED_SELFTEST_PIN 4
#define LED_SELFTEST_COUNT 30

// boot-time baud; the Makefile overrides this with serial.baud from the host
// config. The receiver re-hunts (below) when traffic doesn't decode and
// remembers the rate that worked, so this only sets how fast the first lock
// happens. (Over USB Serial/JTAG the line rate is meaningless — the hunt is a
// no-op there, which is fine.)
#ifndef HOST_BAUD
#define HOST_BAUD 921600
#endif

// blank the strip when the host stops sending (crash, unplug); the last frame
// would otherwise stay lit forever. The Makefile overrides this with
// serial.host_timeout_ms from the host config.
#ifndef HOST_TIMEOUT_MS
#define HOST_TIMEOUT_MS 5000
#endif

// when bytes keep arriving but never form a valid frame, the host is probably
// talking at another rate: step through the candidates (the same set the
// host's baudToSpeed() supports) until frames decode. Even the slowest replay
// shows a frame well inside 500 ms, so that's enough per candidate to
// recognize a lock. A silent line never hunts, so a host that merely stopped
// finds the receiver where it left it.
#define HUNT_AFTER_MS 500
#define HUNT_MIN_BYTES 64

// upper bound on one recording (frameCount*count*3) so a corrupt header can't
// trigger a huge allocation; far above any real animation (~tens of KB).
#define REC_MAX_BYTES (512u * 1024u)

// millis() shim: 32-bit millisecond counter (wraps like Arduino's; every use
// below is unsigned `now - then` difference math, which is wrap-safe).
static inline uint32_t millis()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

const uint32_t BAUDS[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000,
    921600, 1000000, 1500000, 2000000};
const size_t NUM_BAUDS = sizeof BAUDS / sizeof BAUDS[0];

// --- serial link ------------------------------------------------------------
// begin(baud): bring the link up. read(): pull up to maxlen bytes, blocking at
// most `wait` ticks (0 = don't block). setBaud(): change the line rate (UART
// only; a no-op over USB). flushInput(): drop buffered bytes after a rate change.
namespace link
{
#if LINK_UART
static const uart_port_t PORT = UART_NUM_0;

void begin(uint32_t baud)
{
    uart_config_t cfg = {};
    cfg.baud_rate = (int)baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // a generous RX ring so a recording upload (many frames back-to-back)
    // can't outrun us while we copy each into RAM (mirrors the old
    // Serial.setRxBufferSize(1024)). We never transmit, so no TX buffer.
    uart_driver_install(PORT, 2048, 0, 0, nullptr, 0);
    uart_param_config(PORT, &cfg);
    // UART0's default pins are already wired to the USB-serial bridge; keep them.
}

int read(uint8_t* buf, size_t maxlen, TickType_t wait)
{
    int n = uart_read_bytes(PORT, buf, maxlen, wait);
    return n < 0 ? 0 : n;
}

void setBaud(uint32_t baud) { uart_set_baudrate(PORT, baud); }
void flushInput() { uart_flush_input(PORT); }

#else // USB Serial/JTAG (C3/C6/H2/S3 native USB)

void begin(uint32_t /*baud*/)
{
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 256;  // we never really transmit, but 0 is rejected
    cfg.rx_buffer_size = 1024; // matches the old Serial.setRxBufferSize(1024)
    usb_serial_jtag_driver_install(&cfg);
}

int read(uint8_t* buf, size_t maxlen, TickType_t wait)
{
    int n = usb_serial_jtag_read_bytes(buf, maxlen, wait);
    return n < 0 ? 0 : n;
}

void setBaud(uint32_t /*baud*/) {} // USB CDC: line rate is set by the host
void flushInput()
{
    uint8_t sink[64];
    while (usb_serial_jtag_read_bytes(sink, sizeof sink, 0) > 0)
    {
    }
}
#endif
} // namespace link

// --- NVS (the old Preferences store) ----------------------------------------
static nvs_handle_t g_nvs = 0;

static uint32_t prefGetU32(const char* key, uint32_t def)
{
    uint32_t v;
    return nvs_get_u32(g_nvs, key, &v) == ESP_OK ? v : def;
}
static uint16_t prefGetU16(const char* key, uint16_t def)
{
    uint16_t v;
    return nvs_get_u16(g_nvs, key, &v) == ESP_OK ? v : def;
}
static uint8_t prefGetU8(const char* key, uint8_t def)
{
    uint8_t v;
    return nvs_get_u8(g_nvs, key, &v) == ESP_OK ? v : def;
}
static void prefSetU32(const char* key, uint32_t v)
{
    nvs_set_u32(g_nvs, key, v);
    nvs_commit(g_nvs);
}
static void prefSetU16(const char* key, uint16_t v)
{
    nvs_set_u16(g_nvs, key, v);
    nvs_commit(g_nvs);
}
static void prefSetU8(const char* key, uint8_t v)
{
    nvs_set_u8(g_nvs, key, v);
    nvs_commit(g_nvs);
}

led_strip_handle_t strip = nullptr;

uint16_t currentCount = 0;
uint8_t currentPin = 13;

// the OS takes a while to start the daemon, so between power-on and the first
// host frame the receiver replays its stored power-on recording (instead of
// sitting dark). The same player runs the shutdown recording after the host
// sends a shutdown command and exits.
bool hostSeen = false;
bool shuttingDown = false; // replaying the shutdown recording (host has gone)

// the recording currently playing (held in RAM so re-recording its on-flash
// file can never race playback), and the shared stepper that paces it
rec::Recording active;
rec::Player player;

// incoming recording being streamed from the host, accumulated in RAM by the
// shared receiver and committed to flash on END (one write — so flash stalls
// can't drop bytes mid-stream). recSlot is the slot its END must name.
rec::RecordingReceiver recRx;
uint8_t recSlot = 0;

unsigned long lastFrameMs = 0;
unsigned long lastHuntMs = 0;
size_t baudIdx = 0;
bool blanked = false;

// crossfading, owned here (and mirrored by the host's viewer) rather than by
// the daemon: we keep the last frame shown and, when the incoming animation's
// id changes, dissolve the new one in over it with the shared fader. Every
// frame — live (onPixels) or replayed (boot/shutdown recordings) — goes through
// show() below carrying an id, so the boot->live and live->shutdown handoffs are
// just ordinary id changes; there's no "am I replaying?" state to track. Live
// ids ride the wire; replay frames get a reserved per-slot id (proto::ANIM_*),
// which the firmware knows from the slot it's playing (see common/fade.hpp).
//
// All frames here are 8.8 fixed point (count*3 uint16 channel values, see
// common/protocol.hpp). show() only records the newest frame; refreshStrip()
// re-latches the strip every DITHER_REFRESH_US, blending an active dissolve
// and dithering the fraction away per latch — so both the dither *and* a
// crossfade run at strip-refresh rate, not at the (much slower) frame rate.
fade::Fader fader;
uint16_t target[MAX_LEDS * 3];    // newest incoming frame (fade destination)
uint16_t lastShown[MAX_LEDS * 3]; // last blended frame latched (fade source)
uint16_t scratch[MAX_LEDS * 3];   // per-latch blend output / replay decode
uint16_t targetCount = 0;        // LEDs in `target`
bool haveFrame = false;          // refresh only once something arrived
uint32_t ditherTick = 0;         // one step per latch (see common/dither.hpp)
int64_t lastRefreshUs = 0;       // esp_timer time of the last latch
uint16_t lastAnimId = proto::ANIM_NONE; // id of the frame on screen
uint16_t lastShownCount = 0;     // its LED count: a dissolve needs matching geometry
uint16_t replayXms = 0;          // crossfade ms for a shutdown replay (from CMD_SHUTDOWN)

// the last rate that produced valid frames, persisted in NVS so a host baud
// change without a reflash costs one hunt per change, not one per power cycle
uint32_t currentBaud = HOST_BAUD;
uint32_t savedBaud = 0;
uint32_t savedFlashed = 0;

// strip geometry from the last valid host frame, persisted so a never-recorded
// board can still bring the strip up (blank) on the right pin/count before the
// host arrives; a loaded recording's own geometry outranks it
uint16_t savedCount = 0;
uint8_t savedPin = 0;

const char* recPath(uint8_t slot)
{
    return slot == proto::SLOT_SHUTDOWN ? REC_PATH_SHUTDOWN : REC_PATH_POWER_ON;
}

void initStrip(uint16_t count, uint8_t pin)
{
    if (strip)
    {
        led_strip_del(strip);
        strip = nullptr;
    }

    led_strip_config_t sc = {};
    sc.strip_gpio_num = pin;
    sc.max_leds = count;
    sc.led_model = LED_MODEL_WS2812;
    // GRB on the wire: led_strip_set_pixel() takes logical (r,g,b) and reorders
    // per this format, so we pass the host's already-corrected R,G,B bytes
    // as-is — same result the old TYPE_GRB Freenove path produced. (led_strip
    // 2.x field; 3.x renamed this to color_component_format.)
    sc.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    sc.flags.invert_out = false;

    led_strip_rmt_config_t rc = {};
    rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = 10 * 1000 * 1000; // 10 MHz — standard WS2812 timing
    rc.mem_block_symbols = 64;
    rc.flags.with_dma = false;

    // on failure leave strip null; show()/blankStrip() guard on it, so a bad
    // pin/count can't brick the receiver (it just stays dark until a good frame)
    if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK)
    {
        strip = nullptr;
        return;
    }

    currentCount = count;
    currentPin = pin;
}

void blankStrip()
{
    if (!strip)
        return;

    led_strip_clear(strip); // zero every pixel and latch it out
    haveFrame = false;      // nothing to dither; the refresh loop idles dark

    // the strip is black now: record that as the last shown, under a sentinel
    // id, so the next frame (a returning host, or a replay) dissolves up from
    // black rather than snapping
    if (currentCount <= MAX_LEDS)
    {
        memset(lastShown, 0, (size_t)currentCount * 3 * sizeof(uint16_t));
        lastShownCount = currentCount;
    }
    lastAnimId = proto::ANIM_NONE;
}

// the one place pixels reach the hardware: blend any active dissolve over the
// newest frame (both in 8.8), round each channel to the strip's 8 bits under
// this latch's dither threshold, and latch. Runs every DITHER_REFRESH_US from
// loop() — and once from show() so a fresh frame never waits on the timer.
void refreshStrip()
{
    if (!strip || !haveFrame)
        return;

    uint16_t count = targetCount;

    memcpy(scratch, target, (size_t)count * 3 * sizeof(uint16_t));
    fader.apply(scratch, count, millis());

    ditherTick++;

    // latching too slowly to hide the dither? round this latch to nearest
    // instead (see SLOW_LATCH_US)
    bool slow = lastRefreshUs != 0 &&
                esp_timer_get_time() - lastRefreshUs > SLOW_LATCH_US;

    for (uint16_t i = 0; i < count; i++)
    {
        // one threshold for the pixel's three channels, so a dim colour
        // rounds up together and stays on-hue (see common/dither.hpp)
        uint8_t t = slow ? 128 : dither::threshold(ditherTick, i);
        uint8_t c[3];

        for (int k = 0; k < 3; k++)
        {
            // values are ≤ 0xFF00 (code*256), so +t can't exceed 0xFFFF;
            // clamp anyway so an out-of-spec value can't wrap to near-black
            uint32_t s = (uint32_t)scratch[i * 3 + k] + t;
            c[k] = s >= 0xFF00 ? 255 : (uint8_t)(s >> 8);
        }

        led_strip_set_pixel(strip, i, c[0], c[1], c[2]);
    }

    led_strip_refresh(strip);

    // remember the *blended* 8.8 frame (not the dithered 8-bit one) as what's
    // on screen, so a dissolve that starts mid-dissolve seeds cleanly
    memcpy(lastShown, scratch, (size_t)count * 3 * sizeof(uint16_t));
    lastShownCount = count;
    lastRefreshUs = esp_timer_get_time();
}

// the one place a frame reaches the display path. Every source — live frames
// and replayed recordings alike — calls this with the animation's id; when the
// id differs from what's on screen we start a dissolve from the last blended
// frame (as long as the geometry matches), then let refreshStrip() render it.
// That single rule covers live switches, the boot->live handoff and the
// live->shutdown handoff, so no source needs to special-case transitions (see
// common/fade.hpp). `px` is count*3 8.8 channel values.
void show(uint16_t animId, uint16_t count, uint16_t xms, const uint16_t* px)
{
    if (!strip || count > MAX_LEDS)
        return;

    // a new animation, and the held frame still lines up: start the dissolve
    if (animId != lastAnimId && count == lastShownCount)
        fader.begin(lastShown, count, xms, millis());

    lastAnimId = animId;

    if (px != target)
        memcpy(target, px, (size_t)count * 3 * sizeof(uint16_t));
    targetCount = count;
    haveFrame = true;

    refreshStrip(); // latch now; the loop keeps re-latching for the dither
}

// device side of the recording store: LittleFS read/write around the shared
// header codec (common/recording.hpp). The format and field layout live there.
bool loadRecording(uint8_t slot, rec::Recording& out)
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

void saveRecording(uint8_t slot, const rec::Recording& r)
{
    FILE* f = fopen(recPath(slot), "wb");
    if (!f)
        return;

    uint8_t hdr[rec::Recording::kHeaderLen];
    r.encodeHeader(hdr);
    fwrite(hdr, 1, sizeof hdr, f);

    if (!r.data.empty())
        fwrite(r.data.data(), 1, r.data.size(), f);

    fclose(f);
}

// the hash stored in a slot's file, or 0 if missing/invalid; lets begin()
// decide whether an upload is unchanged without reading the whole file
uint32_t storedHash(uint8_t slot)
{
    FILE* f = fopen(recPath(slot), "rb");
    if (!f)
        return 0;

    uint8_t hdr[rec::Recording::kHeaderLen];
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr;
    fclose(f);

    return ok ? rec::Recording::headerHash(hdr) : 0;
}

// start replaying whatever is in `active`, re-initing the strip if the
// recording's geometry differs from what's currently up
void beginReplay()
{
    if (active.valid() &&
        (active.count != currentCount || active.pin != currentPin))
        initStrip(active.count, active.pin);

    player.start(&active);
}

// a validated command frame from the host
void handleCommand(uint8_t cmd, const uint8_t* payload, uint16_t len)
{
    if (cmd == proto::CMD_SHUTDOWN)
    {
        // the host has sent this and exited; replay the stored shutdown
        // recording to completion, then stay dark. hostSeen stops the
        // power-on path from resuming.
        hostSeen = true;
        shuttingDown = true;

        // optional 2-byte payload: how long to dissolve from the last live
        // frame into the shutdown recording (see protocol.hpp). The replay path
        // stamps proto::ANIM_SHUTDOWN on those frames, so the dissolve happens
        // through show() like any other id change — no special-casing here.
        replayXms = len >= 2 ? (uint16_t)(payload[0] | (payload[1] << 8)) : 0;

        if (loadRecording(proto::SLOT_SHUTDOWN, active))
            beginReplay();
        else
            blankStrip(); // nothing recorded yet
    }
    else if (cmd == proto::CMD_REC_BEGIN)
    {
        rec::BeginInfo bi;
        if (bi.decode(payload, len))
        {
            recSlot = bi.slot;
            // unchanged upload (hash matches the stored file): drop it instead
            // of rewriting flash
            recRx.begin(bi, bi.hash == storedHash(bi.slot));
        }
    }
    else if (cmd == proto::CMD_REC_FRAME)
    {
        recRx.frame(payload, len);
    }
    else if (cmd == proto::CMD_REC_END)
    {
        // commit the buffered recording in one write (no mid-stream stalls); a
        // skipped/short/mismatched upload just clears state
        if (recRx.complete() && len >= 1 && payload[0] == recSlot)
            saveRecording(recSlot, recRx.rec);

        recRx.reset();
    }
}

// the host endpoint: a proto::FrameHandler that drives the real strip. The
// shared parser (common/receiver.hpp) does all the AA-55 framing and checksum
// work and calls these when a clean frame lands, so this side only says what a
// frame *means* on the hardware. The same handler interface the on-screen
// viewer implements (host/virtual_strip.cpp).
struct StripHandler : proto::FrameHandler
{
    void onPixels(uint8_t pin, uint16_t count, uint16_t anim, uint16_t xms,
                  const uint16_t* rgb) override
    {
        if (pin != currentPin || count != currentCount)
        {
            // pixels past the new count (or on the old pin) would otherwise
            // latch their last color forever
            if (strip && (count < currentCount || pin != currentPin))
                blankStrip();

            initStrip(count, pin);
        }

        // a switch (the anim id changed) dissolves; a geometry change snaps,
        // since show() only fades when the held frame's count still matches
        show(anim, count, xms, rgb);

        lastFrameMs = millis();
        blanked = false;
        hostSeen = true;

        // the host is (back) in control: drop any standalone replay and free
        // its RAM, and clear the shutdown latch so a stop/start resumes cleanly
        shuttingDown = false;
        player.stop();
        if (active.frameCount)
            active.clear();

        // a checksummed frame proves this rate works; writes happen only when
        // something actually changed, so NVS wear is one write per baud change
        // or reflash
        if (currentBaud != savedBaud || savedFlashed != HOST_BAUD)
        {
            prefSetU32("baud", currentBaud);
            prefSetU32("flashed", HOST_BAUD);
            savedBaud = currentBaud;
            savedFlashed = HOST_BAUD;
        }

        // remember the geometry so a never-recorded board can still bring the
        // strip up before the host arrives next time
        if (currentCount != savedCount || currentPin != savedPin)
        {
            prefSetU16("count", currentCount);
            prefSetU8("pin", currentPin);
            savedCount = currentCount;
            savedPin = currentPin;
        }
    }

    void onCommand(uint8_t cmd, const uint8_t* payload, uint16_t len) override
    {
        handleCommand(cmd, payload, len);
    }
};

StripHandler handler;
proto::Receiver receiver(handler, MAX_LEDS);

void setup()
{
#ifdef LED_SELFTEST
    // hardware proof: cycle the strip red -> green -> blue forever on the
    // wired pin, with nothing else running. If this lights up, the board runs
    // our strip code fine and a dark strip is a daemon-link/config problem, not
    // the chip. If even this stays dark, it's wiring/pin/lib, not the host.
    initStrip(LED_SELFTEST_COUNT, LED_SELFTEST_PIN);
    const uint8_t colors[3][3] = {{40, 0, 0}, {0, 40, 0}, {0, 0, 40}};
    for (uint8_t c = 0;; c = (c + 1) % 3)
    {
        for (uint16_t i = 0; strip && i < LED_SELFTEST_COUNT; i++)
            led_strip_set_pixel(strip, i, colors[c][0], colors[c][1],
                                colors[c][2]);
        if (strip)
            led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
#endif

    // NVS (the Preferences store). Re-init after erasing if the partition is
    // from an old layout or full.
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_open("ledrx", NVS_READWRITE, &g_nvs);

    savedBaud = prefGetU32("baud", 0);
    savedFlashed = prefGetU32("flashed", 0);
    savedCount = prefGetU16("count", 0);
    savedPin = prefGetU8("pin", 13);

    // trust the remembered rate only if the firmware default is the same one
    // it was saved under — a reflash with a new HOST_BAUD means the host
    // config changed, so the new default outranks it
    if (savedBaud != 0 && savedFlashed == HOST_BAUD)
        currentBaud = savedBaud;

    link::begin(currentBaud);

    // align the hunt cycle with the boot baud when it's in the list
    for (size_t i = 0; i < NUM_BAUDS; i++)
        if (BAUDS[i] == currentBaud)
            baudIdx = i;

    recRx.maxBytes = REC_MAX_BYTES;

    // LittleFS on the "storage" partition; format on first boot if unformatted
    esp_vfs_littlefs_conf_t lc = {};
    lc.base_path = LFS_BASE;
    lc.partition_label = "storage";
    lc.format_if_mount_failed = true;
    lc.dont_mount = false;
    esp_vfs_littlefs_register(&lc);

    // start replaying the stored power-on recording immediately; the first host
    // frame drops it and takes over (see onPixels). With no recording yet,
    // bring the strip up blank on the last known geometry so a never-recorded
    // board isn't undefined.
    if (loadRecording(proto::SLOT_POWER_ON, active))
        beginReplay();
    else if (savedCount > 0 && savedCount <= MAX_LEDS)
        initStrip(savedCount, savedPin);
}

void loop()
{
    // drain whatever the link has and push it through the shared parser
    // (common/receiver.hpp); it carries state across these chunks, recognizes
    // complete frames and calls the StripHandler, which lights the strip
    // (onPixels) or acts on a command (onCommand). The first read blocks a
    // couple of ms so an idle loop yields to the RTOS instead of spinning;
    // once bytes arrive we drain the burst with non-blocking reads.
    uint8_t chunk[256];
    TickType_t wait = pdMS_TO_TICKS(2);
    int n;
    do
    {
        n = link::read(chunk, sizeof chunk, wait);
        if (n > 0)
            receiver.feed(chunk, (size_t)n);
        wait = 0;
    } while (n == (int)sizeof chunk);

    unsigned long now = millis();

    // only after the host has talked once, and only when no shutdown replay
    // owns the strip: a silent host that never sent a shutdown command (a
    // crash/unplug) still goes dark on the timeout. The shutdown replay drives
    // its own blank, so don't pre-empt it.
    if (hostSeen && strip && !blanked && !shuttingDown &&
        now - lastFrameMs > HOST_TIMEOUT_MS)
    {
        blankStrip();
        blanked = true;
    }

    unsigned long sinceGood =
        now - (lastHuntMs > lastFrameMs ? lastHuntMs : lastFrameMs);

    // hunt for the host's baud until it's found (including all through the
    // power-on replay — that's exactly when we want to lock on). Don't hunt
    // once the host has intentionally gone (shutdown): there's nothing to lock
    // onto and we'd disturb the animation.
    if (!shuttingDown && receiver.bytesSinceValid() >= HUNT_MIN_BYTES &&
        sinceGood > HUNT_AFTER_MS)
    {
        baudIdx = (baudIdx + 1) % NUM_BAUDS;
        currentBaud = BAUDS[baudIdx];
        link::setBaud(currentBaud);
        link::flushInput();

        // drop any half-frame caught at the old rate and zero the byte counter,
        // so the next hunt is timed from a clean scan
        receiver.reset();
        lastHuntMs = now;
    }

    // the replay owns the strip before the host's first frame (power-on) and
    // during a shutdown after its last. Player::done() holds the final frame.
    if (((!hostSeen) || shuttingDown) && !player.done() && strip)
    {
        if (const uint8_t* px = player.tick(now))
        {
            // recordings store the wire's little-endian 8.8 pixels; decode
            // into the blend scratch (show() copies it on into `target`)
            uint16_t n = active.count <= MAX_LEDS ? active.count : 0;
            for (size_t i = 0; i < (size_t)n * 3; i++)
                scratch[i] = (uint16_t)(px[i * 2] | (px[i * 2 + 1] << 8));

            // stamp the slot's reserved id so the handoff is an ordinary id
            // change: shutdown frames dissolve in over the last live frame
            // (replayXms); boot frames carry no fade of their own (xms 0) — the
            // first live frame dissolves over the boot recording's last frame,
            // using that live frame's own crossfade duration
            if (shuttingDown)
                show(proto::ANIM_SHUTDOWN, n, replayXms, scratch);
            else
                show(proto::ANIM_BOOT, n, 0, scratch);
        }

        // the shutdown recording ends on its own near-black frame; blank once
        // it's done so the strip goes truly dark
        if (player.done() && shuttingDown)
            blankStrip();
    }

    // between frames, keep re-latching the newest (blended) frame with a fresh
    // dither threshold, so sub-code values render as high-rate duty cycles —
    // and an active dissolve advances at this rate too, not at frame rate
    if (haveFrame && strip &&
        esp_timer_get_time() - lastRefreshUs >= DITHER_REFRESH_US)
        refreshStrip();
}

extern "C" void app_main(void)
{
    setup();
    for (;;)
        loop();
}
