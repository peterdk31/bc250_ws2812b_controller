#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "audio.hpp"

// a classic analyzer meter: one bar growing from the start of the strip
// toward the end with the music's loudness, colored chart-style along
// its length (default green -> amber -> red). The bar's contents are
// the spectrum — the base pumps with the bass, the middle carries the
// mids, the tip flickers with the treble — so a bass drop slams the
// base while a hi-hat run makes the tip dance. Riding above the bar is
// the classic flair piece: a peak-hold dot that gets pushed up by every
// swell, hangs for a beat, then falls back under gravity until the next
// hit catches it. Audio levels come from audio::Levels (capture and
// auto-gain shared by the audio family), so the bar fills the strip at
// any volume.
//
// config:
//   palette            bar colors, base -> tip
//                      (default chart green -> amber -> red)
//   peak_color         RRGGBB of the peak-hold dot (default ffffff)
//   peak_hold_seconds  how long the dot hangs before falling (default 0.35)
//   peak_fall_seconds  the dot's full-strip fall time (default 0.9)
//   pump               how strongly band energy lights its stretch of
//                      the bar, 0 flat .. 1 full spectrum (default 0.7)
//   reverse            grow from the other end of the strip (default false)
//   speed              wash drift / shimmer rate multiplier (default 1.0)
//   noise              flow/noise blend for the color wobble (default 0.3)
//   attack_seconds / release_seconds / gain_seconds   as in `audio_music`
class Spectrum : public Effect
{
public:
    ~Spectrum() override
    {
        if (acquired)
            audio::Levels::shared().release();
    }

    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "00d860,ffc020,ff3020"));
        speed = cfg.getFloat("speed", 1.0f);
        noiseMix = cfg.getFloat("noise", 0.3f);

        pump = cfg.getFloat("pump", 0.7f);
        if (pump < 0.0f) pump = 0.0f;
        if (pump > 1.0f) pump = 1.0f;

        holdSec = cfg.getFloat("peak_hold_seconds", 0.35f);
        if (holdSec < 0.0f) holdSec = 0.0f;

        float fallSec = cfg.getFloat("peak_fall_seconds", 0.9f);
        if (fallSec < 0.1f) fallSec = 0.1f;

        // gravity from the full-strip fall time: 1 = g * T^2 / 2
        grav = 2.0f / (fallSec * fallSec);

        reverse = cfg.getBool("reverse", false);

        uint32_t pc = cfg.getColor("peak_color", 0xffffff);
        peakR = (pc >> 16) & 0xFF;
        peakG = (pc >> 8) & 0xFF;
        peakB = pc & 0xFF;

        audio::Levels& lv = audio::Levels::shared();

        lv.configure(cfg.getFloat("attack_seconds", 0.035f),
                     cfg.getFloat("release_seconds", 0.3f),
                     cfg.getFloat("gain_seconds", 6.0f));
        lv.acquire();
        acquired = true;
    }

    void render(Strip& strip, float t) override
    {
        (void)t;

        float dt = frameDelayMs() / 1000.0f;

        audio::Levels& lv = audio::Levels::shared();
        lv.update(dt);

        float lev = lv.level();

        // per-band energy, gently curved so near-silence in a band goes
        // properly dark instead of hovering
        float env[3] = {powf(lv.bass(), 1.3f), powf(lv.mid(), 1.3f),
                        powf(lv.treble(), 1.3f)};

        // the wash quickens with treble sparkle (integrated clock — never
        // scale t directly, a rate change would teleport the pattern)
        wash += dt * (1.0f + 0.8f * lv.treble());

        // peak-hold dot: pushed up instantly by the bar, then it hangs
        // for the hold time and falls back under gravity until the bar
        // catches it again; in silence it settles onto the base
        if (lev >= peak)
        {
            peak = lev;
            peakVel = 0;
            heldFor = 0;
        }
        else
        {
            heldFor += dt;

            if (heldFor > holdSec)
            {
                peakVel += grav * dt;
                peak -= peakVel * dt;
            }

            if (peak < lev)
            {
                peak = lev;
                peakVel = 0;
            }
        }

        int leds = strip.size();

        // the dot straddles about a pixel and a half
        float ps = 0.75f / leds;
        float inv2p2 = 1.0f / (2.0f * ps * ps);

        // the dot fades out as it settles onto the strip's base, so
        // silence ends with a clean dim floor instead of a parked dot
        float dotGain = peak * leds * 0.5f;
        if (dotGain > 1.0f) dotGain = 1.0f;

        for (int i = 0; i < leds; i++)
        {
            // this pixel's center along the bar, 0..1 from its base
            float x = reverse ? (leds - i - 0.5f) / leds
                              : (i + 0.5f) / leds;

            // soft tip: full inside the bar, fading across the single
            // pixel straddling its edge
            float live = (lev - x) * leds + 0.5f;
            if (live < 0.0f) live = 0.0f;
            if (live > 1.0f) live = 1.0f;

            float w = motion::mix(motion::flow(x, wash, speed),
                                  motion::noise(x, wash, speed), noiseMix);
            float v = 0.6f + 0.4f * motion::shimmer(x, wash, speed);

            // chart-style color: position along the bar picks the
            // palette stop, wobbling slightly on the wash so the
            // boundaries breathe instead of sitting pinned
            uint8_t r, g, b;
            palette.at(x + 0.06f * (w - 0.5f), r, g, b);

            // the bar's contents are the spectrum: each stretch of the
            // bar is lit by its own band's energy — bass at the base,
            // mids in the middle, treble at the tip
            float mT = motion::ease((x - 0.5f) / 0.35f);
            float mB = 1.0f - motion::ease((x - 0.1f) / 0.35f);
            float mM = 1.0f - mB - mT;

            float bandE = mB * env[0] + mM * env[1] + mT * env[2];
            float lift = (1.0f - pump) + pump * (0.15f + 0.85f * bandE);

            // the unlit track stays truly dark: a dim wash here would
            // sit below one 8-bit step after gamma, where the
            // receiver's dither reads as jitter on the strip
            float k = v * lift * live;

            float rr = r * k;
            float gg = g * k;
            float bb = b * k;

            // the peak dot, additive over whatever is beneath it
            float dd = x - peak;
            float p = dotGain * expf(-dd * dd * inv2p2);

            if (p > 0.003f)
            {
                rr += peakR * p;
                gg += peakG * p;
                bb += peakB * p;
            }

            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;

            strip.setPixel(i, (uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
        }
    }

private:
    color::Gradient palette;
    float speed = 1.0f;
    float noiseMix = 0.3f;
    float pump = 0.7f;
    float holdSec = 0.35f;
    float grav = 2.0f;
    bool reverse = false;
    float peakR = 255, peakG = 255, peakB = 255;

    bool acquired = false;
    float peak = 0;
    float peakVel = 0;
    float heldFor = 0;
    float wash = 0;
};

REGISTER_EFFECT("audio_spectrum", Spectrum)
