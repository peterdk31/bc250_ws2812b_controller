#include <math.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "steam.hpp"

// progress bar for Steam downloads (steam.hpp), activated by the
// steam_dl condition. The head pixel is antialiased, so even a 10-LED
// strip resolves single percents; the displayed value glides toward
// the target so coarse updates animate instead of jumping.
//
// Read clarity comes first: three plainly distinct zones.
//   * the unfilled track is a dim, steady cool color, so the strip's
//     full length is visible and obviously "not done yet";
//   * the filled bar is a bright warm gradient -- cool (magenta) at the
//     back, hot (gold) at the front -- so the completed amount is
//     unmistakable, and the leading edge is always the hottest pixel;
//   * a bright pulsing glow rides the leading edge, marking exactly how
//     far along the download is.
//
// Motion serves the bar and stays inside the filled region. Gentle waves
// flow toward the head (fluid, low amplitude, so the fill never gaps),
// and every percent bump (steam.hpp advances ~1 Hz while bytes arrive)
// fires a `surge`: the whole bar and head flare, and a bright crest
// rushes from the back up to the head -- a clear "progress!" ripple in
// the same direction as the flow. When it stalls the surge fades and the
// bar settles to a calm flow.
//
// On completion (steam.hpp reports finished) the whole strip pulses
// done_color as a brief celebration before the rule deactivates.
//
// config:
//   palette            filled bar gradient, back -> head = cool -> hot
//                      (default magenta -> orange -> gold)
//   head_color         leading-edge glow + ripple RRGGBB (default warm
//                      white fff2cc)
//   track_color        unfilled track RRGGBB, kept dim (default 0e1c3c)
//   done_color         completion-pulse RRGGBB (default 00ff66)
//   done_pulse_period  completion pulse period in seconds (default 1.4;
//                      0 holds done_color steady instead of pulsing)
//   smoothing_seconds  glide time constant (default 0.4)
//   speed              flow / pulse rate multiplier (default 1.0)
//   ripple_depth       flow-wave depth on the fill, 0..1 (default 0.35;
//                      stays well lit so the fill never reads as gappy)
//   breathe_depth      slow whole-bar breathing, 0..1 (default 0.10)
//   surge_boost        whole-bar brightness flare per progress bump
//                      (default 0.45)
//   surge_ripple       brightness of the crest that rushes to the head
//                      on a progress bump (default 0.9; 0 disables it)
//   ripple_speed       crest travel speed, strips per second (default 1.4)
//   surge_decay        seconds for a surge to fade (default 1.0)
//   head_glow          brightness of the leading-edge glow (default 0.7)
class SteamDownload : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        palette = color::Gradient(cfg.get("palette", "d4009c,ff5a1e,ffd23c"));

        uint32_t head = cfg.getColor("head_color", 0xfff2cc);
        hr = (head >> 16) & 0xFF;
        hg = (head >> 8) & 0xFF;
        hb = head & 0xFF;

        uint32_t track = cfg.getColor("track_color", 0x0e1c3c);
        tr = (track >> 16) & 0xFF;
        tg = (track >> 8) & 0xFF;
        tb = track & 0xFF;

        uint32_t done = cfg.getColor("done_color", 0x00ff66);
        dr = (done >> 16) & 0xFF;
        dg = (done >> 8) & 0xFF;
        db = done & 0xFF;

        donePulse = cfg.getFloat("done_pulse_period", 1.4f);
        smoothing = cfg.getFloat("smoothing_seconds", 0.4f);
        speed = cfg.getFloat("speed", 1.0f);
        rippleDepth = cfg.getFloat("ripple_depth", 0.35f);
        breatheDepth = cfg.getFloat("breathe_depth", 0.10f);
        surgeBoost = cfg.getFloat("surge_boost", 0.45f);
        surgeRipple = cfg.getFloat("surge_ripple", 0.9f);
        rippleSpeed = cfg.getFloat("ripple_speed", 1.4f);
        surgeDecay = cfg.getFloat("surge_decay", 1.0f);
        headGlow = cfg.getFloat("head_glow", 0.7f);

        // seed from the live percentage rather than 0: while a game
        // downloads, proc:steam / cpu_load / gpu_load rules contend for
        // the strip, so this effect is re-activated (and re-init'd) on
        // every bit of churn. Resuming at the current percent makes a
        // re-activation seamless instead of gliding up from empty.
        const steam::Downloads& dl = steam::downloads();

        target = dl.percent / 100.0f;
        shown = target;
        lastSeen = target;
        surge = 0.0f;
        rippleT0 = -1000.0f;
    }

    void render(Strip& strip, float t) override
    {
        const steam::Downloads& dl = steam::downloads();

        if (dl.active)
            target = dl.percent / 100.0f;

        float dt = frameDelayMs() / 1000.0f;
        float alpha = smoothing > 0 ? 1.0f - expf(-dt / smoothing) : 1.0f;

        shown += alpha * (target - shown);

        int leds = strip.size();

        if (dl.finished)
        {
            // download complete: pulse the whole strip in done_color as a
            // brief celebration. steam.hpp holds the rule active for a few
            // seconds past completion so this actually shows.
            float phase = donePulse > 0
                        ? sinf(t * 2.0f * (float)M_PI / donePulse)
                        : 1.0f;
            float v = 0.55f + 0.45f * phase; // pulses 0.10 .. 1.0

            for (int i = 0; i < leds; i++)
                strip.setPixel(i, (uint8_t)(dr * v), (uint8_t)(dg * v),
                               (uint8_t)(db * v));

            return;
        }

        // a progress bump re-arms the surge and launches a fresh crest from
        // the back of the bar. A drop means a new part restarted at 0.
        if (target > lastSeen + 1e-4f)
        {
            surge = 1.0f;
            rippleT0 = t;
            lastSeen = target;
        }
        else if (target < lastSeen)
        {
            lastSeen = target;
        }

        surge *= expf(-dt / surgeDecay);
        if (surge < 0) surge = 0;

        float pos = shown * leds; // head position, in pixels
        int head = (int)pos;
        float frac = pos - head;

        // slow whole-bar breath so the fill is alive between surges
        float breathe = 1.0f;
        if (breatheDepth > 0)
        {
            float ph = 0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI / 4.5f * speed);
            breathe = 1.0f - breatheDepth * ph;
        }

        // whole-bar flare on a progress bump
        float flare = 1.0f + surge * surgeBoost;

        // head-glow pulse, plus extra on a surge
        float headPulse = headGlow * (0.6f + 0.4f * sinf(t * 2.0f * (float)M_PI
                                                         / 1.3f * speed))
                        + surge * 0.7f;

        // the surge crest's position: rushes from the back (0) toward the
        // head, same direction as the flow waves
        float crestPos = (t - rippleT0) * rippleSpeed * leds;

        for (int i = 0; i < leds; i++)
        {
            float center = i + 0.5f;

            // ----- the unfilled track: dim, steady, cool -----
            float eR = tr, eG = tg, eB = tb;

            // ----- the filled bar -----
            // color ramps along the bar, so the back is cool (magenta) and
            // the leading edge is always the hot end (gold)
            float along = pos > 0.5f ? center / pos : 1.0f;
            if (along > 1.0f) along = 1.0f;
            uint8_t cr8, cg8, cb8;
            palette.at(along, cr8, cg8, cb8);

            // fluid flow: two waves drifting toward the head, kept shallow
            // (it never dims the bar enough to look like a gap)
            float x = center / leds;
            float wv = 0.5f + 0.32f * sinf(x * 7.0f - t * 1.4f * speed)
                            + 0.18f * sinf(x * 3.0f - t * 0.9f * speed + 1.3f);
            float flow = (1.0f - rippleDepth) + rippleDepth * wv;

            float b = breathe * flow * flare;
            float fR = cr8 * b, fG = cg8 * b, fB = cb8 * b;

            // surge crest rushing to the head (additive, warm)
            if (surgeRipple > 0)
            {
                float d = center - crestPos;
                float cr = expf(-(d * d) / (2.0f * 1.3f * 1.3f))
                         * surge * surgeRipple;
                fR += hr * cr; fG += hg * cr; fB += hb * cr;
            }

            // bright leading-edge glow (additive)
            float dh = center - pos;
            float g = expf(-(dh * dh) / (2.0f * 1.0f * 1.0f)) * headPulse;
            fR += hr * g; fG += hg * g; fB += hb * g;

            // ----- select zone (everything above is confined to the fill;
            // the head pixel cross-fades fill->track by its fractional part,
            // so nothing spills past the leading edge) -----
            float R, G, B;
            if (i < head)        { R = fR; G = fG; B = fB; }
            else if (i == head)  { R = fR * frac + eR * (1 - frac);
                                   G = fG * frac + eG * (1 - frac);
                                   B = fB * frac + eB * (1 - frac); }
            else                 { R = eR; G = eG; B = eB; }

            strip.setPixel(i, clamp(R), clamp(G), clamp(B));
        }
    }

private:
    static uint8_t clamp(float v)
    {
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return (uint8_t)v;
    }

    color::Gradient palette;
    uint8_t hr = 0xff, hg = 0xf2, hb = 0xcc;
    uint8_t tr = 0x0e, tg = 0x1c, tb = 0x3c;
    uint8_t dr = 0, dg = 255, db = 102;
    float donePulse = 1.4f;
    float smoothing = 0.4f;
    float speed = 1.0f;
    float rippleDepth = 0.35f;
    float breatheDepth = 0.10f;
    float surgeBoost = 0.45f;
    float surgeRipple = 0.9f;
    float rippleSpeed = 1.4f;
    float surgeDecay = 1.0f;
    float headGlow = 0.7f;
    float target = 0.0f;
    float shown = 0.0f;
    float lastSeen = 0.0f;
    float surge = 0.0f;
    float rippleT0 = -1000.0f;
};

REGISTER_EFFECT("steam_download", SteamDownload)
