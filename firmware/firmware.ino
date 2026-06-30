#include <Freenove_WS2812_Lib_for_ESP32.h>
#include <Preferences.h>
#include <LittleFS.h>

// shared with the host (copied into src/ by `make receiver`): the wire
// protocol, the streaming frame parser, and the recording format + replay
// logic. The receiver renders no effects — the daemon records the power-on/
// shutdown animations to frames and streams them here (CMD_REC_*); this
// firmware stores them in flash and replays them. So an effect tweak no longer
// means a reflash; it's picked up on the next daemon start and shown one power
// cycle later. The only device-specific parts left are LittleFS save/load, the
// WS2812 output, the UART, and millis() — everything else is common/recording.hpp.
#include "src/protocol.hpp"
#include "src/receiver.hpp"
#include "src/recording.hpp"
#include "src/fade.hpp"

#define CHANNEL 0
#define MAX_LEDS 2048

// TEMP diagnostic: uncomment to bypass the whole host/recording path and drive
// the strip directly from setup(). Proves the C3 + Freenove lib + our
// initStrip()/show() actually light the strip on this pin, independent of the
// daemon link. Set the pin/count to your wiring. Remove (or recomment) and
// reflash once confirmed. See the LED_SELFTEST block in setup().
// #define LED_SELFTEST
#define LED_SELFTEST_PIN 4
#define LED_SELFTEST_COUNT 30

// boot-time baud; `make flash` overrides this with serial.baud from the host
// config. The receiver re-hunts (below) when traffic doesn't decode and
// remembers the rate that worked, so this only sets how fast the first lock
// happens
#ifndef HOST_BAUD
#define HOST_BAUD 921600
#endif

// blank the strip when the host stops sending (crash, unplug); the last frame
// would otherwise stay lit forever. `make flash` overrides this with
// serial.host_timeout_ms from the host config
#ifndef HOST_TIMEOUT_MS
#define HOST_TIMEOUT_MS 5000
#endif

// when bytes keep arriving but never form a valid frame, the host is probably
// talking at another rate: step through the candidates (the same set the
// host's baudToSpeed() supports) until frames decode. Even the slowest replay
// shows a frame well inside 500 ms, so that's enough per candidate to
// recognize a lock. A silent line never hunts, so a host that merely stopped
// finds the receiver where it left it
#define HUNT_AFTER_MS 500
#define HUNT_MIN_BYTES 64

// the recording files live here; one per slot (see common/protocol.hpp)
#define REC_PATH_POWER_ON "/boot.rec"
#define REC_PATH_SHUTDOWN "/shutdown.rec"

// upper bound on one recording (frameCount*count*3) so a corrupt header can't
// trigger a huge allocation; far above any real animation (~tens of KB)
#define REC_MAX_BYTES (512u * 1024u)

const uint32_t BAUDS[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000,
    921600, 1000000, 1500000, 2000000};
const size_t NUM_BAUDS = sizeof BAUDS / sizeof BAUDS[0];

Freenove_ESP32_WS2812 *strip = nullptr;

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
// can't drop UART bytes mid-stream). recSlot is the slot its END must name.
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
// show() below carrying an id, so the boot→live and live→shutdown handoffs are
// just ordinary id changes; there's no "am I replaying?" state to track. Live
// ids ride the wire; replay frames get a reserved per-slot id (proto::ANIM_*),
// which the firmware knows from the slot it's playing (see common/fade.hpp).
fade::Fader fader;
uint8_t lastShown[MAX_LEDS * 3]; // last frame written to the strip
uint8_t frameBuf[MAX_LEDS * 3];  // scratch: the incoming frame, blended in place
uint16_t lastAnimId = proto::ANIM_NONE; // id of the frame on screen
uint16_t lastShownCount = 0;     // its LED count: a dissolve needs matching geometry
uint16_t replayXms = 0;          // crossfade ms for a shutdown replay (from CMD_SHUTDOWN)

// the last rate that produced valid frames, persisted in NVS so a host baud
// change without a reflash costs one hunt per change, not one per power cycle
Preferences prefs;
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
        delete strip;
        strip = nullptr;
    }

    strip = new Freenove_ESP32_WS2812(count, pin, CHANNEL, TYPE_GRB);
    strip->begin();

    currentCount = count;
    currentPin = pin;
}

void blankStrip()
{
    if (!strip)
        return;

    for (uint16_t i = 0; i < currentCount; i++)
        strip->setLedColorData(i, 0, 0, 0);

    strip->show();

    // the strip is black now: record that as the last shown, under a sentinel
    // id, so the next frame (a returning host, or a replay) dissolves up from
    // black rather than snapping
    if ((size_t)currentCount * 3 <= sizeof lastShown)
    {
        memset(lastShown, 0, (size_t)currentCount * 3);
        lastShownCount = currentCount;
    }
    lastAnimId = proto::ANIM_NONE;
}

// the one place a frame reaches the strip. Every source — live frames and
// replayed recordings alike — calls this with the animation's id; when the id
// differs from what's on screen we dissolve the new frame in over the last
// (as long as the geometry matches), then remember it. That single rule covers
// live switches, the boot→live handoff and the live→shutdown handoff, so no
// source needs to special-case transitions (see common/fade.hpp).
void show(uint16_t animId, uint16_t count, uint16_t xms, const uint8_t* px)
{
    if (!strip || (size_t)count * 3 > sizeof frameBuf)
        return;

    uint32_t now = millis();

    // a new animation, and the held frame still lines up: start the dissolve
    if (animId != lastAnimId && count == lastShownCount)
        fader.begin(lastShown, count, xms, now);

    lastAnimId = animId;

    memcpy(frameBuf, px, (size_t)count * 3);
    fader.apply(frameBuf, count, now);

    for (uint16_t i = 0; i < count; i++)
        strip->setLedColorData(i, frameBuf[i * 3], frameBuf[i * 3 + 1],
                               frameBuf[i * 3 + 2]);
    strip->show();

    memcpy(lastShown, frameBuf, (size_t)count * 3);
    lastShownCount = count;
}

// device side of the recording store: LittleFS read/write around the shared
// header codec (common/recording.hpp). The format and field layout live there.
bool loadRecording(uint8_t slot, rec::Recording& out)
{
    File f = LittleFS.open(recPath(slot), "r");
    if (!f)
        return false;

    uint8_t hdr[rec::Recording::kHeaderLen];
    if (f.read(hdr, sizeof hdr) != sizeof hdr || !out.decodeHeader(hdr))
    {
        f.close();
        return false;
    }

    size_t bytes = (size_t)out.frameCount * out.frameBytes();
    if (out.count == 0 || out.count > MAX_LEDS || out.frameCount == 0 ||
        bytes > REC_MAX_BYTES)
    {
        f.close();
        return false;
    }

    out.data.resize(bytes);
    bool ok = f.read(out.data.data(), bytes) == bytes;
    f.close();

    if (!ok)
    {
        out.clear();
        return false;
    }

    return true;
}

void saveRecording(uint8_t slot, const rec::Recording& r)
{
    File f = LittleFS.open(recPath(slot), "w");
    if (!f)
        return;

    uint8_t hdr[rec::Recording::kHeaderLen];
    r.encodeHeader(hdr);
    f.write(hdr, sizeof hdr);

    if (!r.data.empty())
        f.write(r.data.data(), r.data.size());

    f.close();
}

// the hash stored in a slot's file, or 0 if missing/invalid; lets begin()
// decide whether an upload is unchanged without reading the whole file
uint32_t storedHash(uint8_t slot)
{
    File f = LittleFS.open(recPath(slot), "r");
    if (!f)
        return 0;

    uint8_t hdr[rec::Recording::kHeaderLen];
    bool ok = f.read(hdr, sizeof hdr) == sizeof hdr;
    f.close();

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
// shared parser (src/receiver.hpp) does all the AA-55 framing and checksum
// work and calls these when a clean frame lands, so this side only says what a
// frame *means* on the hardware. The same handler interface the on-screen
// viewer implements (host/virtual_strip.cpp).
struct StripHandler : proto::FrameHandler
{
    void onPixels(uint8_t pin, uint16_t count, uint16_t anim, uint16_t xms,
                  const uint8_t* rgb) override
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
            prefs.putUInt("baud", currentBaud);
            prefs.putUInt("flashed", HOST_BAUD);
            savedBaud = currentBaud;
            savedFlashed = HOST_BAUD;
        }

        // remember the geometry so a never-recorded board can still bring the
        // strip up before the host arrives next time
        if (currentCount != savedCount || currentPin != savedPin)
        {
            prefs.putUShort("count", currentCount);
            prefs.putUChar("pin", currentPin);
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
    // wired pin, with nothing else running. If this lights up, the C3 runs our
    // strip code fine and the dark strip is a daemon-link/config problem, not
    // the chip. If even this stays dark, it's wiring/pin/lib, not the host.
    initStrip(LED_SELFTEST_COUNT, LED_SELFTEST_PIN);
    const uint8_t colors[3][3] = {{40, 0, 0}, {0, 40, 0}, {0, 0, 40}};
    for (uint8_t c = 0;; c = (c + 1) % 3)
    {
        for (uint16_t i = 0; i < LED_SELFTEST_COUNT; i++)
            strip->setLedColorData(i, colors[c][0], colors[c][1], colors[c][2]);
        strip->show();
        delay(700);
    }
#endif

    prefs.begin("ledrx", false);
    savedBaud = prefs.getUInt("baud", 0);
    savedFlashed = prefs.getUInt("flashed", 0);
    savedCount = prefs.getUShort("count", 0);
    savedPin = prefs.getUChar("pin", 13);

    // trust the remembered rate only if the firmware default is the same one
    // it was saved under — a reflash with a new HOST_BAUD means the host
    // config changed, so the new default outranks it
    if (savedBaud != 0 && savedFlashed == HOST_BAUD)
        currentBaud = savedBaud;

    // a bigger RX buffer so a recording upload (many frames back-to-back)
    // can't outrun us while we copy each frame into RAM
    Serial.setRxBufferSize(1024);
    Serial.begin(currentBaud);

    // align the hunt cycle with the boot baud when it's in the list
    for (size_t i = 0; i < NUM_BAUDS; i++)
        if (BAUDS[i] == currentBaud)
            baudIdx = i;

    recRx.maxBytes = REC_MAX_BYTES;

    LittleFS.begin(true); // format on first boot if there's no filesystem yet

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
    // drain whatever the UART has and push it through the shared parser
    // (src/receiver.hpp); it carries state across these chunks, recognizes
    // complete frames and calls the StripHandler, which lights the strip
    // (onPixels) or acts on a command (onCommand).
    while (Serial.available())
    {
        uint8_t chunk[256];
        int avail = Serial.available();
        int want = avail < (int)sizeof chunk ? avail : (int)sizeof chunk;
        int n = Serial.readBytes(chunk, want);

        if (n > 0)
            receiver.feed(chunk, (size_t)n);
    }

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
        Serial.updateBaudRate(currentBaud);

        while (Serial.available())
            Serial.read();

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
            // stamp the slot's reserved id so the handoff is an ordinary id
            // change: shutdown frames dissolve in over the last live frame
            // (replayXms); boot frames carry no fade of their own (xms 0) — the
            // first live frame dissolves over the boot recording's last frame,
            // using that live frame's own crossfade duration
            if (shuttingDown)
                show(proto::ANIM_SHUTDOWN, active.count, replayXms, px);
            else
                show(proto::ANIM_BOOT, active.count, 0, px);
        }

        // the shutdown recording ends on its own near-black frame; blank once
        // it's done so the strip goes truly dark
        if (player.done() && shuttingDown)
            blankStrip();
    }
}
