#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "hwmon.hpp"

// CPU and GPU load as two soft bars growing from the center of the strip
// outward — CPU toward pixel 0, GPU toward the last pixel. Instead of a
// hard thermal ramp, the lit pixels are washed with slow aurora-style
// color curtains plus a gentle brightness shimmer, so a bar pinned at
// 100% (a GPU under a game) keeps drifting and breathing rather than
// sitting as a static block. The bar tip fades across a pixel instead of
// snapping on and off.
//
// On top of the wash each bar carries a heartbeat: a soft crest born at
// the center that travels out along the lit bar and dies at the tip,
// beating slowly at idle and quickening with that side's load — so the
// two halves visibly beat at their own pace. A sudden load jump deepens
// the beat for a second or so (the surge), making launches and spikes
// land as a felt kick rather than just a longer bar.
//
// The wash doesn't walk the whole palette: each side samples a sliding
// *window* of it, and the window's position is that side's load — idle
// bars wash in the palette's cool first stops, a pinned GPU burns in its
// hot last ones while an idle CPU side stays cool, so the color says "how
// hard" while the bar length says "how much". On top of the load, the
// window wanders a little on a minutes-scale noise walk, so a long gaming
// session drifts through neighbouring hues instead of sitting on the same
// three forever. heat_span 1.0 opens the window to the whole palette and
// restores the old fixed-wash behaviour.
//
// config:
//   smoothing_seconds  load smoothing time constant (default 0.7)
//   palette            comma-separated stops, cool (idle) -> hot (full
//                      load) (default "00e0c0,2060ff,a040ff,ff4060,ff9000",
//                      teal -> blue -> violet -> magenta -> orange)
//   heat_span          fraction of the palette the wash spans at any
//                      moment (default 0.45); the rest is the travel the
//                      window slides as load rises. 1.0 = whole palette,
//                      no sliding
//   color_drift        how far the window wanders off the pure load
//                      position on a minutes-scale walk, in palette
//                      fractions (default 0.3); 0 pins color to load
//   speed              drift / shimmer rate multiplier (default 1.4)
//   noise              flow/noise blend 0 (sine flow) .. 1 (noise) (default 0.3)
//   pulse              heartbeat strength, 0 disables (default 0.5)
//   pulse_width        crest half-width as a fraction of the half-strip
//                      (default 0.16)
//   pulse_rate_idle    beats per second at zero load (default 0.5)
//   pulse_rate_full    beats per second at full load (default 1.8)
//   pulse_color        RRGGBB the crest leans toward (default ffffff)
//   surge              extra beat depth per unit of load jump (default 1.5)
class Load : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        smoothing = cfg.getFloat("smoothing_seconds", 0.7f);
        palette = color::Gradient(cfg.get(
            "palette", "00e0c0,2060ff,a040ff,ff4060,ff9000"));
        heatSpan = cfg.getFloat("heat_span", 0.45f);
        if (heatSpan < 0.05f) heatSpan = 0.05f;
        if (heatSpan > 1.0f) heatSpan = 1.0f;
        colorDrift = cfg.getFloat("color_drift", 0.3f);
        if (colorDrift < 0.0f) colorDrift = 0.0f;
        speed = cfg.getFloat("speed", 1.4f);
        noiseMix = cfg.getFloat("noise", 0.3f);

        pulseStrength = cfg.getFloat("pulse", 0.5f);
        pulseWidth = cfg.getFloat("pulse_width", 0.16f);
        if (pulseWidth < 0.02f) pulseWidth = 0.02f;
        rateIdle = cfg.getFloat("pulse_rate_idle", 0.5f);
        rateFull = cfg.getFloat("pulse_rate_full", 1.8f);
        surgeGain = cfg.getFloat("surge", 1.5f);

        uint32_t pc = cfg.getColor("pulse_color", 0xffffff);
        pulseR = (pc >> 16) & 0xFF;
        pulseG = (pc >> 8) & 0xFF;
        pulseB = pc & 0xFF;

        if (!gpuLoad.available())
            fprintf(stderr, "load: no gpu load source found yet, "
                            "will keep looking\n");

        hwmon::readCpuCounters(prevBusy, prevTotal);
    }

    void render(Strip& strip, float t) override
    {
        float dt = frameDelayMs() / 1000.0f;
        float alpha = smoothing > 0 ? 1.0f - expf(-dt / smoothing) : 1.0f;

        float prevCpu = cpu, prevGpu = gpu;

        cpu += alpha * (readCpuLoad() - cpu);
        gpu += alpha * (readGpuLoad() - gpu);

        // each side's heartbeat phase integrates a load-dependent rate,
        // so the beat quickens and slows without ever skipping
        cpuPhase += dt * (rateIdle + (rateFull - rateIdle) * cpu);
        gpuPhase += dt * (rateIdle + (rateFull - rateIdle) * gpu);

        // upward load jumps feed the surge, which decays over ~a second;
        // the smoothed load spreads a step over smoothing_seconds, so the
        // surge sums to roughly surgeGain * (size of the jump)
        float decay = expf(-dt / 1.2f);
        cpuSurge = cpuSurge * decay + fmaxf(cpu - prevCpu, 0.0f) * surgeGain;
        gpuSurge = gpuSurge * decay + fmaxf(gpu - prevGpu, 0.0f) * surgeGain;

        // crest position along its journey (0 center .. 1 tip) and beat
        // depth; the half-sine envelope births the crest softly at the
        // center and lets it die out just as it reaches the tip
        float crestC = cpuPhase - floorf(cpuPhase);
        float crestG = gpuPhase - floorf(gpuPhase);
        float beatC = sinf(3.14159265f * crestC)
            * fminf(pulseStrength + fminf(cpuSurge, 1.0f), 1.5f);
        float beatG = sinf(3.14159265f * crestG)
            * fminf(pulseStrength + fminf(gpuSurge, 1.0f), 1.5f);

        float inv2w2 = 1.0f / (2.0f * pulseWidth * pulseWidth);

        // each side's palette window: load slides it toward the hot end,
        // a minutes-scale wander (a distinct noise track per side) keeps a
        // long steady session drifting through neighbouring hues
        float baseC = windowBase(cpu, 0.0f, t);
        float baseG = windowBase(gpu, 40.0f, t);

        int leds = strip.size();
        float half = leds / 2.0f;

        for (int i = 0; i < leds; i++)
        {
            bool cpuSide = i < half;

            // this pixel's center as distance from the strip center, 0..1
            float d = cpuSide ? (half - i - 0.5f) / half
                              : (i + 0.5f - half) / half;

            float level = cpuSide ? cpu : gpu;

            // soft tip: full inside the bar, fading over the single pixel
            // straddling its end (distance in pixels is d * half). `live`
            // is the same ramp before flooring: 1 inside the bar, 0 deep in
            // the unlit track, crossing 0..1 across the tip pixel.
            float live = (level - d) * half + 0.5f;
            if (live < 0.0f) live = 0.0f;
            if (live > 1.0f) live = 1.0f;

            // the unlit track stays truly dark — a faint wash there would
            // sit below one 8-bit step after gamma, where the receiver's
            // dither reads as jitter on the strip
            float fill = live;

            // spatial coordinate for the wash/shimmer mirrored about the
            // center (distance outward, 0..1), so both halves flow
            // symmetrically rather than the pattern sweeping straight
            // across the strip in one screen direction
            float x = d;

            // aurora-style wash walking the palette: a flow field blended
            // with value noise, so the color drifts and never repeats
            float w = motion::mix(motion::flow(x, t, speed),
                                  motion::noise(x, t, speed), noiseMix);

            // a separate slow field shimmers the brightness, so even a
            // full bar is always gently moving
            float v = 0.6f + 0.4f * motion::shimmer(x, t, speed);

            uint8_t r, g, b;
            palette.at((cpuSide ? baseC : baseG) + w * heatSpan, r, g, b);

            // the heartbeat crest travelling out from the center along
            // this side's lit bar; `live` confines it to the bar and
            // fades it across the tip pixel with everything else
            float crest = cpuSide ? crestC : crestG;
            float beat = cpuSide ? beatC : beatG;
            float dd = d - crest * level;
            float p = beat * expf(-dd * dd * inv2w2) * live;

            // the crest lifts brightness and leans the hue toward
            // pulse_color, so it reads as energy moving through the wash
            float lift = p > 1.0f ? 1.0f : p;
            float rr = r + (pulseR - r) * lift * 0.6f;
            float gg = g + (pulseG - g) * lift * 0.6f;
            float bb = b + (pulseB - b) * lift * 0.6f;

            float k = v * (1.0f + p);
            if (k > 1.0f) k = 1.0f;
            k *= fill;

            strip.setPixel(i, (uint8_t)(rr * k), (uint8_t)(gg * k),
                           (uint8_t)(bb * k));
        }
    }

private:
    // busy share of /proc/stat ticks since the previous call, 0..1
    float readCpuLoad()
    {
        unsigned long long busy, total;

        if (!hwmon::readCpuCounters(busy, total) || total <= prevTotal)
            return cpu;

        float load = (float)(busy - prevBusy) / (float)(total - prevTotal);

        prevBusy = busy;
        prevTotal = total;

        return load < 0 ? 0 : load > 1 ? 1 : load;
    }

    float readGpuLoad()
    {
        return gpuLoad.readPercent() / 100;
    }

    // start of a side's palette window, 0 .. 1-heatSpan: its load plus the
    // slow wander, saturating at the palette's ends rather than wrapping.
    // noise speed 0.08 puts a full wander cell at ~80 s — minutes-scale.
    float windowBase(float load, float track, float t) const
    {
        float h = load + (motion::noise(track, t, 0.08f) - 0.5f) * colorDrift;
        if (h < 0.0f) h = 0.0f;
        if (h > 1.0f) h = 1.0f;
        return h * (1.0f - heatSpan);
    }

    float smoothing = 0.7f;
    color::Gradient palette;
    float heatSpan = 0.45f;
    float colorDrift = 0.3f;
    float speed = 1.4f;
    float noiseMix = 0.3f;
    float pulseStrength = 0.5f;
    float pulseWidth = 0.16f;
    float rateIdle = 0.5f;
    float rateFull = 1.8f;
    float surgeGain = 1.5f;
    float pulseR = 255, pulseG = 255, pulseB = 255;
    float cpu = 0;
    float gpu = 0;
    float cpuPhase = 0;
    float gpuPhase = 0;
    float cpuSurge = 0;
    float gpuSurge = 0;

    hwmon::GpuLoad gpuLoad;
    unsigned long long prevBusy = 0;
    unsigned long long prevTotal = 0;
};

REGISTER_EFFECT("load", Load)
