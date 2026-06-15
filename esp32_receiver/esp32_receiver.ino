#include <Freenove_WS2812_Lib_for_ESP32.h>
#include <Preferences.h>

#define CHANNEL 0
#define MAX_LEDS 2048

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

// the host applies gamma, white balance and brightness before sending,
// so the standalone animation must do the same or it would blast
// full-power LEDs into a room tuned for strip.brightness. `make flash`
// overrides brightness and white balance with the host config's values
#ifndef STRIP_BRIGHTNESS
#define STRIP_BRIGHTNESS 0.2f
#endif
#define STRIP_GAMMA 2.2f

// per-channel linear gain as 0xRRGGBB; 0xFFFFFF disables. Mirrors
// strip.white_balance so the standalone breathe is tinted like the
// daemon's frames (see ws2812_serial.hpp). Single gamma here is fine:
// the breathe is one fixed amber, not a gradient
#ifndef STRIP_WHITE_BALANCE
#define STRIP_WHITE_BALANCE 0xFFFFFF
#endif

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

// the OS takes a while to start the daemon, so between power-on and
// the first valid frame the receiver breathes on its own (the host's
// idle warm amber) instead of sitting dark. Never resumes after host
// loss — a host that stops after having talked means shutdown or
// crash, where dark is the right answer
bool hostSeen = false;
unsigned long lastBreathMs = 0;

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
// baud so the power-on breathe lights the right pixels on the right
// pin even when the flashed defaults are stale
uint16_t savedCount = 0;
uint8_t savedPin = 0;

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

// the same curve the host's lut applies (ws2812_serial.hpp): gamma,
// then the per-channel white-balance gain and brightness, so the
// standalone breathe matches what the daemon will render later
uint8_t shade(uint8_t c, float v, float gain)
{
    return (uint8_t)(powf(c * v / 255.0f, STRIP_GAMMA)
                     * gain * STRIP_BRIGHTNESS * 255.0f + 0.5f);
}

// the host's idle "breathe" effect: warm amber, 5 s sine, 5% floor
void renderBreath(unsigned long now)
{
    float t = now / 1000.0f;
    float breath = 0.5f - 0.5f * cosf(t * 2.0f * PI / 5.0f);
    float v = 0.05f + 0.95f * breath;

    uint8_t r = shade(255, v, ((STRIP_WHITE_BALANCE >> 16) & 0xFF) / 255.0f);
    uint8_t g = shade(120, v, ((STRIP_WHITE_BALANCE >> 8)  & 0xFF) / 255.0f);
    uint8_t b = shade(24,  v, ( STRIP_WHITE_BALANCE        & 0xFF) / 255.0f);

    for (uint16_t i = 0; i < currentCount; i++)
        strip->setLedColorData(i, r, g, b);

    strip->show();
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

    // bring the strip up immediately for the power-on breathe; the
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

        if (Serial.read() != 0xAA)
            continue;

        uint8_t sync;

        if (!readExact(&sync, 1))
            break;

        if (sync != 0x55)
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

        // remember the geometry for the next power-on breathe
        if (currentCount != savedCount || currentPin != savedPin)
        {
            prefs.putUShort("count", currentCount);
            prefs.putUChar("pin", currentPin);
            savedCount = currentCount;
            savedPin = currentPin;
        }
    }

    unsigned long now = millis();

    // only after the host has talked once: before that the power-on
    // breathe owns the strip and there is nothing stale to blank
    if (hostSeen && strip && !blanked &&
        now - lastFrameMs > HOST_TIMEOUT_MS)
    {
        blankStrip();
        blanked = true;
    }

    unsigned long sinceGood =
        now - (lastHuntMs > lastFrameMs ? lastHuntMs : lastFrameMs);

    if (bytesSinceValid >= HUNT_MIN_BYTES &&
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

    if (!hostSeen && strip && now - lastBreathMs >= 33)
    {
        lastBreathMs = now;
        renderBreath(now);
    }
}
