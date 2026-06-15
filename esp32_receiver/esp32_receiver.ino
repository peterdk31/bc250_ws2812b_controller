#include <Freenove_WS2812_Lib_for_ESP32.h>
#include <Preferences.h>

// shared with the host (copied into src/ by `make receiver`): the wire
// protocol, the color LUT, and the actual effect code so the receiver's
// standalone animations are the same effects the daemon runs, not a
// hand-kept copy. ESP32_BUILD (set by the Makefile) selects the
// receiver-side Strip/EffectConfig backends inside effect.hpp.
#include "src/protocol.hpp"
#include "src/effect.hpp"

#define CHANNEL 0
#define MAX_LEDS 2048

// cold-start defaults for the power-on and shutdown effects, used until
// the daemon pushes the configured ones (the config.json "esp32" block)
// over serial, which the receiver then remembers in NVS. So a fresh,
// never-driven board shows these; after the first daemon run it uses the
// configured effects. Any shared/effects/* name works
#ifndef DEFAULT_POWER_ON_EFFECT
#define DEFAULT_POWER_ON_EFFECT "boot"
#endif
#ifndef DEFAULT_SHUTDOWN_EFFECT
#define DEFAULT_SHUTDOWN_EFFECT "shutdown"
#endif

// boot-time baud; `make flash` overrides this with serial.baud from
// the host config. The receiver re-hunts (below) when traffic doesn't
// decode and remembers the rate that worked, so this only determines
// how fast the first-ever lock happens
#ifndef HOST_BAUD
#define HOST_BAUD 921600
#endif

// blank the strip when the host stops sending (crash, unplug); the
// last frame would otherwise stay lit forever. `make flash` overrides
// this with serial.host_timeout_ms from the host config
#ifndef HOST_TIMEOUT_MS
#define HOST_TIMEOUT_MS 5000
#endif

// strip geometry for the standalone power-on animation, before the
// host has said anything. `make flash` overrides these with
// strip.leds/strip.pin from the host config; geometry from the last
// valid frame (saved in NVS) outranks both. A count of 0 disables the
// animation until the first frame ever
#ifndef DEFAULT_LED_COUNT
#define DEFAULT_LED_COUNT 0
#endif
#ifndef DEFAULT_LED_PIN
#define DEFAULT_LED_PIN 13
#endif

// the strip-level correction (brightness/white balance/gamma) the host
// applies before sending lives in esp32_strip.hpp now, shared with the
// daemon and baked in by `make flash`.

// when bytes keep arriving but never form a valid frame, the host is
// probably talking at another rate: step through the candidates (the
// same set the host's baudToSpeed() supports) until frames decode.
// Even the slowest effect sends a frame every 200 ms, so 500 ms per
// candidate is enough to recognize a lock. A silent line never hunts,
// so a host that merely stopped finds the receiver where it left it
#define HUNT_AFTER_MS 500
#define HUNT_MIN_BYTES 64

const uint32_t BAUDS[] = {
    9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000,
    921600, 1000000, 1500000, 2000000};
const size_t NUM_BAUDS = sizeof BAUDS / sizeof BAUDS[0];

Freenove_ESP32_WS2812 *strip = nullptr;

uint16_t currentCount = 0;
uint8_t currentPin = 13;

uint8_t *frameBuffer = nullptr;
size_t frameBufferSize = 0;

// the OS takes a while to start the daemon, so between power-on and the
// first valid frame the receiver runs an effect on its own (the configured
// power-on slot, or DEFAULT_POWER_ON_EFFECT) instead of sitting dark. The
// same machinery plays the shutdown effect after the host sends a shutdown
// command and exits.
bool hostSeen = false;

// the standalone animation currently playing (power-on or shutdown), the
// shared color LUT it renders through, and its timing. `shuttingDown`
// marks the post-host shutdown effect; `standaloneDone` latches once a
// standalone effect finishes so it isn't auto-restarted (the strip holds
// its last frame), until a host frame takes over again.
ColorLut standaloneLut;
std::unique_ptr<Effect> standalone;
unsigned long standaloneStartMs = 0;
unsigned long lastStandaloneMs = 0;
bool shuttingDown = false;
bool standaloneDone = false;

unsigned long lastFrameMs = 0;
unsigned long lastHuntMs = 0;
uint32_t bytesSinceValid = 0;
size_t baudIdx = 0;
bool blanked = false;

// the last rate that produced valid frames, persisted in NVS so a
// host baud change without a reflash costs one hunt per change, not
// one per power cycle
Preferences prefs;
uint32_t currentBaud = HOST_BAUD;
uint32_t savedBaud = 0;
uint32_t savedFlashed = 0;

// strip geometry from the last valid frame, persisted alongside the
// baud so the power-on effect lights the right pixels on the right
// pin even when the flashed defaults are stale
uint16_t savedCount = 0;
uint8_t savedPin = 0;

// one configured slot: which effect, and its settings as key=value pairs.
// Defined here, above the first function, because arduino-cli emits its
// generated prototypes there — including ones that return/take SlotConfig
// — so the type must be visible before that point.
struct SlotConfig
{
    std::string effect;
    EffectConfig::Settings settings;
};

void initStrip(uint16_t count, uint8_t pin)
{
    if (strip)
    {
        delete strip;
        strip = nullptr;
    }

    strip = new Freenove_ESP32_WS2812(
        count,
        pin,
        CHANNEL,
        TYPE_GRB);

    strip->begin();

    currentCount = count;
    currentPin = pin;
}

bool readExact(uint8_t *buffer, size_t len)
{
    size_t received = 0;

    while (received < len)
    {
        int n = Serial.readBytes(
            buffer + received,
            len - received);

        if (n <= 0)
            return false;

        received += n;
        bytesSinceValid += n;
    }

    return true;
}

void blankStrip()
{
    if (!strip)
        return;

    for (uint16_t i = 0; i < currentCount; i++)
        strip->setLedColorData(i, 0, 0, 0);

    strip->show();
}

// parse a slot string (effect name on the first line, then key=value
// lines — see protocol.hpp). A blank/missing effect line falls back to
// the given default
SlotConfig parseSlot(const std::string& s, const char* fallback)
{
    SlotConfig c;
    size_t start = 0;
    bool firstLine = true;

    while (start <= s.size())
    {
        size_t nl = s.find('\n', start);
        std::string line =
            s.substr(start, nl == std::string::npos ? std::string::npos
                                                    : nl - start);

        if (firstLine)
        {
            c.effect = line;
            firstLine = false;
        }
        else if (!line.empty())
        {
            size_t eq = line.find('=');
            if (eq != std::string::npos)
                c.settings.emplace_back(line.substr(0, eq),
                                        line.substr(eq + 1));
        }

        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }

    if (c.effect.empty())
        c.effect = fallback;

    return c;
}

// the slot the daemon stored in NVS (key "po" or "sd"), or the cold-start
// default if the host hasn't pushed config yet
SlotConfig loadSlot(const char* key, const char* fallback)
{
    return parseSlot(prefs.getString(key, "").c_str(), fallback);
}

// begin a standalone effect, rendered through the shared LUT with the
// given settings. The settings only need to outlive init(), which copies
// what the effect needs, so a caller can pass a temporary
void startStandalone(const SlotConfig& slot, unsigned long now)
{
    standalone = createEffect(slot.effect);
    standaloneStartMs = now;
    lastStandaloneMs = 0; // render the first frame immediately

    if (standalone)
        standalone->init(EffectConfig(slot.settings), currentCount);
}

// advance the standalone effect one frame if its frame delay has elapsed;
// when it reports finished, stop and hold (a looping effect never does)
void tickStandalone(unsigned long now)
{
    if (!standalone || !strip)
        return;

    if (lastStandaloneMs != 0 &&
        now - lastStandaloneMs < (unsigned long)standalone->frameDelayMs())
        return;

    lastStandaloneMs = now;

    Esp32Strip sink(strip, currentCount, standaloneLut);
    sink.beginFrame();
    standalone->render(sink, (now - standaloneStartMs) / 1000.0f);
    strip->show();

    if (!standalone->finished())
        return;

    // the effect has played once. The shutdown effect ends on a blanked
    // strip; a finite power-on effect just stops on its last frame (boot
    // ends dark). Either way, don't auto-start anything else — a user who
    // wants e.g. boot-then-idle composes that into one effect, or sets a
    // looping power-on effect. A host frame clears this and takes over.
    if (shuttingDown)
        blankStrip();

    standalone.reset();
    standaloneDone = true;
}

// a validated command frame from the host
void handleCommand(uint8_t cmd, const uint8_t* payload, uint16_t len)
{
    if (cmd == proto::CMD_SHUTDOWN)
    {
        // the host has sent this and exited; play the configured shutdown
        // effect (remembered in NVS) to completion, then stay dark.
        // hostSeen stops the power-on path from resuming; clearing
        // standaloneDone lets shutdown run even if a finite power-on
        // effect had already finished
        hostSeen = true;
        shuttingDown = true;
        standaloneDone = false;
        startStandalone(loadSlot("sd", DEFAULT_SHUTDOWN_EFFECT), millis());
    }
    else if (cmd == proto::CMD_CONFIG)
    {
        // payload: power-on slot string '\0' shutdown slot string '\0'.
        // terminate defensively so a malformed payload can't run off the
        // buffer, then remember both for the next power-on and shutdown
        if (len < frameBufferSize)
            frameBuffer[len] = 0;

        const char* po = (const char*)payload;
        uint16_t poLen = 0;
        while (poLen < len && payload[poLen])
            poLen++;
        const char* sd = (poLen < len) ? (const char*)payload + poLen + 1 : "";

        prefs.putString("po", po);
        prefs.putString("sd", sd);
    }
}

void setup()
{
    prefs.begin("ledrx", false);
    savedBaud = prefs.getUInt("baud", 0);
    savedFlashed = prefs.getUInt("flashed", 0);
    savedCount = prefs.getUShort("count", 0);
    savedPin = prefs.getUChar("pin", DEFAULT_LED_PIN);

    // trust the remembered rate only if the firmware default is the
    // same one it was saved under — a reflash with a new HOST_BAUD
    // means the host config changed, so the new default outranks it
    if (savedBaud != 0 && savedFlashed == HOST_BAUD)
        currentBaud = savedBaud;

    Serial.begin(currentBaud);

    // align the hunt cycle with the boot baud when it's in the list
    for (size_t i = 0; i < NUM_BAUDS; i++)
        if (BAUDS[i] == currentBaud)
            baudIdx = i;

    frameBufferSize = MAX_LEDS * 3;
    frameBuffer = (uint8_t *)malloc(frameBufferSize);

    // the standalone animations render through the same correction the
    // host bakes into its frames, so they match (values from the flash
    // config; see esp32_strip.hpp)
    standaloneLut.setGamma(STRIP_GAMMA);
    standaloneLut.setWhiteBalance(STRIP_WHITE_BALANCE);
    standaloneLut.setBrightness(STRIP_BRIGHTNESS);

    // bring the strip up immediately for the power-on animation; the
    // first host frame re-inits if the geometry changed meanwhile
    uint16_t bootCount = savedCount ? savedCount : DEFAULT_LED_COUNT;

    if (bootCount > 0 && bootCount <= MAX_LEDS)
        initStrip(bootCount, savedPin);
}

void loop()
{
    while (Serial.available())
    {
        bytesSinceValid++;

        if (Serial.read() != proto::SYNC0)
            continue;

        uint8_t sync;

        if (!readExact(&sync, 1))
            break;

        // command frame: cmd, a little-endian length, the payload, then a
        // checksum over all of those (see protocol.hpp)
        if (sync == proto::CMD_SYNC)
        {
            uint8_t hdr[3]; // cmd, len_lo, len_hi

            if (!readExact(hdr, 3))
                break;

            uint8_t cmd = hdr[0];
            uint16_t len = hdr[1] | (hdr[2] << 8);

            // a bogus length (noise) would stall readExact; drop and resync
            if (len > frameBufferSize)
                continue;

            if (len && !readExact(frameBuffer, len))
                break;

            uint8_t checksum;

            if (!readExact(&checksum, 1))
                break;

            uint8_t expected = cmd ^ hdr[1] ^ hdr[2];
            for (uint16_t i = 0; i < len; i++)
                expected ^= frameBuffer[i];

            if (checksum != expected)
                continue;

            bytesSinceValid = 0;
            handleCommand(cmd, frameBuffer, len);
            continue;
        }

        if (sync != proto::SYNC1)
            continue;

        uint8_t header[3];

        if (!readExact(header, 3))
            break;

        uint8_t pin = header[0];

        uint16_t ledCount =
            header[1] |
            (header[2] << 8);

        if (ledCount == 0 || ledCount > MAX_LEDS)
            continue;

        size_t rgbBytes = ledCount * 3;

        if (!readExact(frameBuffer, rgbBytes))
            break;

        uint8_t checksum;

        if (!readExact(&checksum, 1))
            break;

        // XOR of pin, count and pixel bytes; a mismatch means we
        // latched onto pixel data that looked like a header — drop the
        // frame and rescan
        uint8_t expected = header[0] ^ header[1] ^ header[2];

        for (size_t i = 0; i < rgbBytes; i++)
            expected ^= frameBuffer[i];

        if (checksum != expected)
            continue;

        if (pin != currentPin ||
            ledCount != currentCount)
        {
            // pixels past the new count (or on the old pin) would
            // otherwise latch their last color forever
            if (strip &&
                (ledCount < currentCount || pin != currentPin))
            {
                blankStrip();
            }

            initStrip(ledCount, pin);
        }

        for (uint16_t i = 0; i < ledCount; i++)
        {
            uint8_t *p = &frameBuffer[i * 3];

            strip->setLedColorData(
                i,
                p[0],
                p[1],
                p[2]);
        }

        strip->show();

        lastFrameMs = millis();
        bytesSinceValid = 0;
        blanked = false;
        hostSeen = true;

        // the host is (back) in control: drop any standalone animation
        // and clear the shutdown latch so a stop/start resumes cleanly
        shuttingDown = false;
        standaloneDone = false;
        if (standalone)
            standalone.reset();

        // a checksummed frame proves this rate works; writes happen
        // only when something actually changed, so NVS wear is one
        // write per baud change or reflash
        if (currentBaud != savedBaud || savedFlashed != HOST_BAUD)
        {
            prefs.putUInt("baud", currentBaud);
            prefs.putUInt("flashed", HOST_BAUD);
            savedBaud = currentBaud;
            savedFlashed = HOST_BAUD;
        }

        // remember the geometry for the next power-on effect
        if (currentCount != savedCount || currentPin != savedPin)
        {
            prefs.putUShort("count", currentCount);
            prefs.putUChar("pin", currentPin);
            savedCount = currentCount;
            savedPin = currentPin;
        }
    }

    unsigned long now = millis();

    // only after the host has talked once, and only when no standalone
    // animation owns the strip: a silent host that never sent a shutdown
    // command (a crash/unplug) still goes dark on the timeout. The
    // shutdown effect drives its own blank, so don't pre-empt it
    if (hostSeen && strip && !blanked && !shuttingDown && !standaloneDone &&
        now - lastFrameMs > HOST_TIMEOUT_MS)
    {
        blankStrip();
        blanked = true;
    }

    unsigned long sinceGood =
        now - (lastHuntMs > lastFrameMs ? lastHuntMs : lastFrameMs);

    // don't hunt for a baud once the host has intentionally gone (shutdown):
    // there's nothing to lock onto and we'd disturb the animation
    if (!shuttingDown && !standaloneDone &&
        bytesSinceValid >= HUNT_MIN_BYTES &&
        sinceGood > HUNT_AFTER_MS)
    {
        baudIdx = (baudIdx + 1) % NUM_BAUDS;
        currentBaud = BAUDS[baudIdx];
        Serial.updateBaudRate(currentBaud);

        while (Serial.available())
            Serial.read();

        bytesSinceValid = 0;
        lastHuntMs = now;
    }

    // standalone animations own the strip before the host's first frame
    // (power-on) and during a shutdown after its last
    if (((!hostSeen) || shuttingDown) && !standaloneDone && strip)
    {
        if (!standalone && !hostSeen)
            startStandalone(loadSlot("po", DEFAULT_POWER_ON_EFFECT), now);

        tickStandalone(now);
    }
}
