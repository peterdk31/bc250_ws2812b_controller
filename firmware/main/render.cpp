#include "render.hpp"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "led_strip.h"

#include "protocol.hpp"
#include "fade.hpp"
#include "dither.hpp"

#include "util.hpp"

// how often the strip is re-latched between host frames. Frames arrive as 8.8
// fixed-point values (see common/protocol.hpp); every latch rounds them to
// the strip's 8 bits with a fresh ordered threshold (common/dither.hpp), so a
// fractional value renders as a duty cycle between adjacent codes. The
// ~500-900 Hz this period yields (the serial read's ~1 ms block and the RMT
// transfer time set the real cadence) is the dither's whole budget: the
// blink-rate floor below can only keep fractions whose pulse rate clears
// DITHER_MIN_BLINK_HZ, so a faster latch means finer fractions survive. A
// long strip's RMT time (~30 µs/LED) throttles this naturally; the floor
// adapts (see refresh), so slow latches degrade to rounding, not flicker.
#define DITHER_REFRESH_US 1000

// the slowest pulse rate a dithered fraction may produce. A duty cycle's
// pulse rate is fraction × latch rate: exact on average, but a near-code
// value pulses *slowly* — value 2.02 shows code 3 six times a second, and at
// dim codes each pulse is a huge relative step (code 2→3 is +50%), so it
// reads as twinkling, not as an average. Fractions whose pulse rate (or gap
// rate, for near-1 fractions) would land below this floor round to the
// nearest code instead: a steady, slightly-off level in place of a slow
// blink. The floor is computed from the actual latch gap, so a starved loop
// or a very long strip degrades gracefully toward plain rounding — this also
// replaces the old fixed slow-latch cutoff.
#define DITHER_MIN_BLINK_HZ 100

namespace render
{
static led_strip_handle_t strip = nullptr;

static uint16_t currentCount = 0;
static uint8_t currentPin = 13;

static fade::Fader fader;
static uint16_t target[MAX_LEDS * 3];    // newest incoming frame (fade destination)
static uint16_t lastShown[MAX_LEDS * 3]; // last blended frame latched (fade source)
static uint16_t scratch[MAX_LEDS * 3];   // per-latch blend output / replay decode
static uint16_t targetCount = 0;        // LEDs in `target`
static bool haveFrame = false;          // refresh only once something arrived
static uint32_t ditherTick = 0;         // one step per latch (see common/dither.hpp)
static int64_t lastRefreshUs = 0;       // esp_timer time of the last latch
static uint16_t lastAnimId = proto::ANIM_NONE; // id of the frame on screen
static uint16_t lastShownCount = 0;     // its LED count: a dissolve needs matching geometry

bool up() { return strip != nullptr; }
uint16_t count() { return currentCount; }
uint8_t pin() { return currentPin; }
uint16_t* decodeBuf() { return scratch; }

void init(uint16_t count, uint8_t pin)
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

    // on failure leave strip null; show()/blank() guard on it, so a bad
    // pin/count can't brick the receiver (it just stays dark until a good frame)
    if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK)
    {
        strip = nullptr;
        return;
    }

    currentCount = count;
    currentPin = pin;
}

void blank()
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
// this latch's dither threshold, and latch. Runs every DITHER_REFRESH_US via
// tick() — and once from show() so a fresh frame never waits on the timer.
static void refresh()
{
    if (!strip || !haveFrame)
        return;

    uint16_t count = targetCount;

    memcpy(scratch, target, (size_t)count * 3 * sizeof(uint16_t));
    fader.apply(scratch, count, millis());

    ditherTick++;

    // the smallest fraction (in 1/256ths) whose pulse rate at the current
    // latch cadence still clears DITHER_MIN_BLINK_HZ; fractions closer to a
    // code than this round to it instead of dithering. The previous latch gap
    // is the best predictor of the next one; at 128 the band is empty and
    // every value rounds to nearest (the old slow-latch behavior).
    uint32_t dtUs = lastRefreshUs != 0
                        ? (uint32_t)(esp_timer_get_time() - lastRefreshUs)
                        : DITHER_REFRESH_US;
    uint32_t fmin =
        (uint32_t)((uint64_t)dtUs * DITHER_MIN_BLINK_HZ * 256 / 1000000);
    if (fmin > 128)
        fmin = 128;

    for (uint16_t i = 0; i < count; i++)
    {
        // one threshold for the pixel's three channels, so a dim colour
        // rounds up together and stays on-hue (see common/dither.hpp)
        uint8_t t = dither::threshold(ditherTick, i);
        uint8_t c[3];

        for (int k = 0; k < 3; k++)
        {
            uint32_t v = scratch[i * 3 + k];
            uint32_t f = v & 0xFF;

            // dither only fractions that pulse above the blink floor — and
            // never duty-cycle against true black: a sub-code value (below
            // code 1) would flash an unlit LED at 100% contrast, which the
            // eye catches even at rates that pass between two lit codes (a
            // pale diffuser makes it glaring; a dark one merely hides it).
            // Everything else rounds to nearest — a steady level, the same
            // thing the host viewer shows.
            uint32_t s;
            if (v < 0x100 || f < fmin || f >= 256 - fmin)
                s = v + 128;
            else
                s = v + t;

            // values are ≤ 0xFF00 (code*256), so +t can't exceed 0xFFFF;
            // clamp anyway so an out-of-spec value can't wrap to near-black
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

    refresh(); // latch now; tick() keeps re-latching for the dither
}

void tick()
{
    if (haveFrame && strip &&
        esp_timer_get_time() - lastRefreshUs >= DITHER_REFRESH_US)
        refresh();
}

void selftest(uint16_t count, uint8_t pin)
{
    // If this lights up, the board runs our strip code fine and a dark strip
    // is a daemon-link/config problem, not the chip. If even this stays dark,
    // it's wiring/pin/lib, not the host.
    init(count, pin);
    const uint8_t colors[3][3] = {{40, 0, 0}, {0, 40, 0}, {0, 0, 40}};
    for (uint8_t c = 0;; c = (c + 1) % 3)
    {
        for (uint16_t i = 0; strip && i < count; i++)
            led_strip_set_pixel(strip, i, colors[c][0], colors[c][1],
                                colors[c][2]);
        if (strip)
            led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}
} // namespace render
