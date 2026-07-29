#include "led_service.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// shared with the host (compiled straight from ../../common, on the include
// path): the wire protocol, the streaming frame parser, and the recording
// format + replay logic.
#include "protocol.hpp"
#include "receiver.hpp"
#include "recording.hpp"

#include "dbglog.hpp"
#include "hostreq.hpp"
#include "link.hpp"
#include "prefs.hpp"
#include "rec_store.hpp"
#include "render.hpp"
#include "util.hpp"

// TEMP diagnostic: uncomment to bypass the whole host/recording path and drive
// the strip directly at task start. Proves the board + led_strip + our
// render code actually light the strip on this pin, independent of the
// daemon link. Set the pin/count to your wiring. Recomment and reflash once
// confirmed. See render::selftest.
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

// host-restart detection (USB Serial/JTAG links only): a running host's USB
// controller sends SOF keepalives every millisecond, and the driver exposes
// their presence as usb_serial_jtag_is_connected(). When they stop for this
// long the host is powered off or rebooting — the receiver itself usually
// stays powered (standby VBUS), so without this it would never replay the
// power-on recording again. When the keepalives return after such a gap, the
// loop re-arms the boot replay as if freshly reset. Long enough to ride out
// brief bus resets (re-enumeration takes ~1 s); a real power-down is minutes.
#define HOST_GONE_MS 3000

// when bytes keep arriving but never form a valid frame, the host is probably
// talking at another rate: step through the candidates (the same set the
// host's baudToSpeed() supports) until frames decode. Even the slowest replay
// shows a frame well inside 500 ms, so that's enough per candidate to
// recognize a lock. A silent line never hunts, so a host that merely stopped
// finds the receiver where it left it.
//
// "valid frame" means any decoded frame — a command counts as much as a pixel
// frame, since both are checksummed and so both prove the rate. That matters
// because the daemon's first traffic after it starts is a recording upload
// (hundreds of CMD_REC_FRAMEs, no pixels yet): when only pixel frames counted,
// a receiver that hadn't seen one yet hunted every HUNT_AFTER_MS straight
// through the upload, and each hunt flushes the input and drops the frame being
// parsed — so the recording never arrived complete and no boot or shutdown
// animation was ever stored.
#define HUNT_AFTER_MS 500
#define HUNT_MIN_BYTES 64

// receiver-side debug log (drained to journalctl when the daemon has
// "sinks.serial.debug_log" on; see dbglog.hpp). Event lines only — nothing
// per-frame — so an unwatched board costs a bounded vsnprintf per event.
#define LLOG(fmt, ...) dbglog::line("led: " fmt, ##__VA_ARGS__)

namespace led
{
static const uint32_t BAUDS[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000,
    921600, 1000000, 1500000, 2000000};
static const size_t NUM_BAUDS = sizeof BAUDS / sizeof BAUDS[0];

// the OS takes a while to start the daemon, so between power-on and the first
// host frame the receiver replays its stored power-on recording (instead of
// sitting dark). The same player runs the shutdown recording after the host
// sends a shutdown command and exits.
static bool hostSeen = false;
static bool shuttingDown = false; // replaying the shutdown recording (host has gone)

// host-restart detection state (see HOST_GONE_MS; only moves on USB links)
static uint32_t usbSilentSince = 0; // millis() when SOF keepalives stopped (0 = present)
static bool usbHostLost = false;    // keepalives have been gone > HOST_GONE_MS

// the recording currently playing (held in RAM so re-recording its on-flash
// file can never race playback), and the shared stepper that paces it
static rec::Recording active;
static rec::Player player;

// incoming recording being streamed from the host, accumulated in RAM by the
// shared receiver and committed to flash on END (one write — so flash stalls
// can't drop bytes mid-stream). recSlot is the slot its END must name.
static rec::RecordingReceiver recRx;
static uint8_t recSlot = 0;

static unsigned long lastFrameMs = 0; // last live *pixel* frame (host liveness)
static unsigned long lastValidMs = 0; // last decoded frame of any kind (baud lock)
static unsigned long lastHuntMs = 0;
static size_t baudIdx = 0;
static bool blanked = false;

static uint16_t replayXms = 0; // crossfade ms for a shutdown replay (from CMD_SHUTDOWN)

// the last rate that produced valid frames, persisted in NVS so a host baud
// change without a reflash costs one hunt per change, not one per power cycle
static uint32_t currentBaud = HOST_BAUD;
static uint32_t savedBaud = 0;
static uint32_t savedFlashed = 0;

// strip geometry from the last valid host frame, persisted so a never-recorded
// board can still bring the strip up (blank) on the right pin/count before the
// host arrives; a loaded recording's own geometry outranks it
static uint16_t savedCount = 0;
static uint8_t savedPin = 0;

// start replaying whatever is in `active`, re-initing the strip if the
// recording's geometry differs from what's currently up
static void beginReplay()
{
    if (active.valid() &&
        (active.count != render::count() || active.pin != render::pin()))
        render::init(active.count, active.pin);

    player.start(&active);
}

// a validated command frame from the host
static void handleCommand(uint8_t cmd, const uint8_t* payload, uint16_t len)
{
    if (cmd == proto::CMD_SHUTDOWN)
    {
        // the host has sent this and exited; replay the stored shutdown
        // recording to completion, then stay dark. hostSeen stops the
        // power-on path from resuming.
        hostSeen = true;
        shuttingDown = true;

        // if the power switch asked for this shutdown, it just got its answer:
        // the host is going down, which is the whole of what it asked. Stops the
        // repeat (and the "nobody answered" report) even if the explicit ack was
        // lost or never made it out before systemd stopped the daemon.
        hostreq::satisfy(proto::REQ_HOST_SHUTDOWN);

        // optional 2-byte payload: how long to dissolve from the last live
        // frame into the shutdown recording (see protocol.hpp). The replay path
        // stamps proto::ANIM_SHUTDOWN on those frames, so the dissolve happens
        // through render::show() like any other id change — no special-casing
        // here.
        replayXms = len >= 2 ? (uint16_t)(payload[0] | (payload[1] << 8)) : 0;

        if (rec_store::load(proto::SLOT_SHUTDOWN, active))
        {
            LLOG("shutdown: replaying %u frames, xfade %u ms",
                 (unsigned)active.frameCount, (unsigned)replayXms);
            beginReplay();
        }
        else
        {
            LLOG("shutdown: no recording stored, blanking");
            render::blank(); // nothing recorded yet
        }
    }
    else if (cmd == proto::CMD_REC_BEGIN)
    {
        rec::BeginInfo bi;
        if (bi.decode(payload, len))
        {
            recSlot = bi.slot;
            // unchanged upload (hash matches the stored file): drop it instead
            // of rewriting flash
            bool skip = bi.hash == rec_store::storedHash(bi.slot);
            recRx.begin(bi, skip);

            LLOG("upload slot %u: %u frames x %u leds%s", (unsigned)bi.slot,
                 (unsigned)bi.frameCount, (unsigned)bi.count,
                 skip ? " (unchanged, skipped)" : "");
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
        {
            bool ok = rec_store::save(recSlot, recRx.rec);
            LLOG("upload slot %u: %s", (unsigned)recSlot,
                 ok ? "stored" : "FLASH WRITE FAILED");
        }
        else if (recRx.storing)
        {
            // bytes went missing between BEGIN and END — the stream was
            // interrupted (see the baud-hunt note above) or the frames were
            // malformed. The slot keeps whatever it had.
            LLOG("upload slot %u: INCOMPLETE, %u/%u bytes — not stored",
                 (unsigned)recSlot, (unsigned)recRx.rec.data.size(),
                 (unsigned)((size_t)recRx.rec.frameCount
                            * recRx.rec.frameBytes()));
        }

        recRx.reset();
    }
    else if (cmd == proto::CMD_LOG_DRAIN)
    {
        // debug backchannel: reply with the buffered log lines the host hasn't
        // seen. Payload is the host's highest-seen seq (little-endian, 0 = all).
        uint32_t since = len >= 4
            ? (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
              | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24)
            : 0;
        dbglog::onDrain(since);
    }
    else if (cmd == proto::CMD_REQ_ACK)
    {
        // the host heard a request of ours (hostreq.hpp) — payload echoes the
        // req code and nonce
        hostreq::onAck(payload, len);
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
        if (pin != render::pin() || count != render::count())
        {
            // pixels past the new count (or on the old pin) would otherwise
            // latch their last color forever
            if (render::up() && (count < render::count() || pin != render::pin()))
                render::blank();

            render::init(count, pin);
        }

        // a switch (the anim id changed) dissolves; a geometry change snaps,
        // since show() only fades when the held frame's count still matches
        render::show(anim, count, xms, rgb);

        lastFrameMs = lastValidMs = millis();
        blanked = false;
        hostSeen = true;

        // a live frame is the strongest "host is here" signal; drop any
        // pending host-restart re-arm so it can't hijack the stream
        usbHostLost = false;
        usbSilentSince = 0;

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
            prefs::setU32("baud", currentBaud);
            prefs::setU32("flashed", HOST_BAUD);
            savedBaud = currentBaud;
            savedFlashed = HOST_BAUD;
        }

        // remember the geometry so a never-recorded board can still bring the
        // strip up before the host arrives next time
        if (render::count() != savedCount || render::pin() != savedPin)
        {
            prefs::setU16("count", render::count());
            prefs::setU8("pin", render::pin());
            savedCount = render::count();
            savedPin = render::pin();
        }
    }

    void onCommand(uint8_t cmd, const uint8_t* payload, uint16_t len) override
    {
        // a checksummed command decoded, so the rate is right — this is the
        // only proof of that during a recording upload, which is all the host
        // sends before its first pixel frame (see HUNT_AFTER_MS)
        lastValidMs = millis();

        handleCommand(cmd, payload, len);
    }
};

static StripHandler handler;
static proto::Receiver receiver(handler, MAX_LEDS);

static void init()
{
#ifdef LED_SELFTEST
    render::selftest(LED_SELFTEST_COUNT, LED_SELFTEST_PIN); // never returns
#endif

    prefs::init("ledrx");

    savedBaud = prefs::getU32("baud", 0);
    savedFlashed = prefs::getU32("flashed", 0);
    savedCount = prefs::getU16("count", 0);
    savedPin = prefs::getU8("pin", 13);

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

    // start replaying the stored power-on recording immediately; the first host
    // frame drops it and takes over (see onPixels). With no recording yet,
    // bring the strip up blank on the last known geometry so a never-recorded
    // board isn't undefined.
    if (rec_store::load(proto::SLOT_POWER_ON, active))
    {
        LLOG("boot: replaying %u frames x %u leds on pin %u",
             (unsigned)active.frameCount, (unsigned)active.count,
             (unsigned)active.pin);
        beginReplay();
    }
    else if (savedCount > 0 && savedCount <= MAX_LEDS)
    {
        LLOG("boot: no recording stored; strip up blank (%u leds, pin %u)",
             (unsigned)savedCount, (unsigned)savedPin);

        render::init(savedCount, savedPin);

        // and actually latch the zeros out: a WS2812 holds its last frame
        // through a reset of *this* chip, so without this the strip would sit
        // lit with whatever the previous session left — and nothing would ever
        // clear it (the host timeout below only arms once a host has been
        // seen, and there's no recording to play over it)
        render::blank();
    }
}

static void loop()
{
    // drain whatever the link has and push it through the shared parser
    // (common/receiver.hpp); it carries state across these chunks, recognizes
    // complete frames and calls the StripHandler, which lights the strip
    // (onPixels) or acts on a command (onCommand). The first read blocks one
    // tick so an idle loop yields to the RTOS instead of spinning — kept to
    // 1 ms because this block plus the RMT transfer is the latch cadence, and
    // the dither's blink floor eats less resolution the faster we latch (see
    // DITHER_REFRESH_US in render.cpp). Once bytes arrive we drain the burst
    // with non-blocking reads.
    uint8_t chunk[256];
    TickType_t wait = pdMS_TO_TICKS(1);
    int n;
    do
    {
        n = link::read(chunk, sizeof chunk, wait);
        if (n > 0)
            receiver.feed(chunk, (size_t)n);
        wait = 0;
    } while (n == (int)sizeof chunk);

    unsigned long now = millis();

    // the receiver→host request channel (hostreq.hpp). This task owns link
    // writes, so an outstanding request — the power switch asking the host to
    // shut itself down — is transmitted from here and nowhere else. A no-op
    // unless one is pending, which is approximately always.
    hostreq::tick((uint32_t)now);

    // notice the host going away and coming back (USB links; see HOST_GONE_MS).
    // When it returns after a real absence, re-arm the power-on replay exactly
    // as if this chip had been reset: a host restart shows the boot animation
    // even though the receiver never lost power. Gated on hostSeen so a cold
    // boot — where the replay is already running while the host is still
    // enumerating — isn't restarted midway.
    if (!link::hostPresent())
    {
        if (usbSilentSince == 0)
            usbSilentSince = now ? now : 1;
        else if (!usbHostLost && now - usbSilentSince > HOST_GONE_MS)
        {
            usbHostLost = true;
            LLOG("host bus quiet %u ms — host is off or rebooting",
                 (unsigned)(now - usbSilentSince));
        }
    }
    else
    {
        usbSilentSince = 0;

        if (usbHostLost)
        {
            usbHostLost = false;

            if (hostSeen)
            {
                hostSeen = false;
                shuttingDown = false;
                blanked = false;
                replayXms = 0;

                // drop any half-parsed leftovers from the previous session
                receiver.reset();
                link::flushInput();

                if (rec_store::load(proto::SLOT_POWER_ON, active))
                {
                    LLOG("host back — replaying boot recording");
                    beginReplay();
                }
                else
                {
                    // as after a reset with no recording: dark until the daemon
                    // takes over, never a stale frame left hanging (the timeout
                    // above is disarmed now that hostSeen is false again)
                    LLOG("host back — no boot recording stored, blanking");
                    render::blank();
                }
            }
        }
    }

    // only after the host has talked once, and only when no shutdown replay
    // owns the strip: a silent host that never sent a shutdown command (a
    // crash/unplug) still goes dark on the timeout. The shutdown replay drives
    // its own blank, so don't pre-empt it.
    if (hostSeen && render::up() && !blanked && !shuttingDown &&
        now - lastFrameMs > HOST_TIMEOUT_MS)
    {
        LLOG("host silent %u ms, blanking", (unsigned)(now - lastFrameMs));
        render::blank();
        blanked = true;
    }

    unsigned long sinceGood =
        now - (lastHuntMs > lastValidMs ? lastHuntMs : lastValidMs);

    // hunt for the host's baud until it's found (including all through the
    // power-on replay — that's exactly when we want to lock on). Don't hunt
    // once the host has intentionally gone (shutdown): there's nothing to lock
    // onto and we'd disturb the animation. And never over USB, where the rate
    // isn't ours (link::rateSelectable) — there the hunt could only ever
    // corrupt a stream that was already decoding fine.
    if (link::rateSelectable() && !shuttingDown &&
        receiver.bytesSinceValid() >= HUNT_MIN_BYTES &&
        sinceGood > HUNT_AFTER_MS)
    {
        baudIdx = (baudIdx + 1) % NUM_BAUDS;
        currentBaud = BAUDS[baudIdx];
        LLOG("baud hunt: trying %u", (unsigned)currentBaud);
        link::setBaud(currentBaud);
        link::flushInput();

        // drop any half-frame caught at the old rate and zero the byte counter,
        // so the next hunt is timed from a clean scan
        receiver.reset();
        lastHuntMs = now;
    }

    // the replay owns the strip before the host's first frame (power-on) and
    // during a shutdown after its last. Player::done() holds the final frame.
    if (((!hostSeen) || shuttingDown) && !player.done() && render::up())
    {
        if (const uint8_t* px = player.tick(now))
        {
            // recordings store the wire's little-endian 8.8 pixels; decode
            // into the render blend buffer (show() copies it on into its
            // fade target)
            uint16_t* dec = render::decodeBuf();
            uint16_t n = active.count <= MAX_LEDS ? active.count : 0;
            for (size_t i = 0; i < (size_t)n * 3; i++)
                dec[i] = (uint16_t)(px[i * 2] | (px[i * 2 + 1] << 8));

            // stamp the slot's reserved id so the handoff is an ordinary id
            // change: shutdown frames dissolve in over the last live frame
            // (replayXms); boot frames carry no fade of their own (xms 0) — the
            // first live frame dissolves over the boot recording's last frame,
            // using that live frame's own crossfade duration
            if (shuttingDown)
                render::show(proto::ANIM_SHUTDOWN, n, replayXms, dec);
            else
                render::show(proto::ANIM_BOOT, n, 0, dec);
        }

        // the shutdown recording ends on its own near-black frame; blank once
        // it's done so the strip goes truly dark
        if (player.done() && shuttingDown)
            render::blank();
    }

    // between frames, keep re-latching the newest (blended) frame with a fresh
    // dither threshold (see render::tick)
    render::tick();
}

static void taskMain(void*)
{
    init();
    for (;;)
        loop();
}

void start()
{
    // its own task, so the latch cadence — the 1 ms blocking read in loop()
    // plus the RMT transfer, which is the dither's whole budget — never waits
    // on other features running in their own tasks. Priority above the default
    // main task so a busy sibling can't starve the latch; anything that needs
    // to influence the LEDs should queue to this task, not poke its state.
    xTaskCreate(taskMain, "led_rx", 6144, nullptr, 5, nullptr);
}
} // namespace led
