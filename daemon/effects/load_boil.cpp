#include <math.h>
#include <stdio.h>
#include "effect.hpp"
#include "color.hpp"
#include "motion.hpp"
#include "hwmon.hpp"

// CPU and GPU load as a lava lamp whose *vigor* is the load: each half of
// the strip (CPU toward pixel 0, GPU toward the last pixel, matching the
// `load` bars) holds a couple of metaball blobs, and that side's smoothed
// load drives how hard its half boils — blob speed and radius breathing
// both scale with load, and each side's palette window slides toward the
// hot end of a cool->hot palette. Idle is one-or-two lazy cool blobs per
// side barely moving; a game launch makes the GPU half boil in hot colors
// while the CPU half stays a slow simmer. Blob tails cross the center, so
// under heavy load the two sides' blobs kiss and bloom at the middle.
// Less precise than `load`'s bars, much more alive — a companion, not a
// replacement.
//
// The blob field, per-blob color blending and white-hot overlap bloom are
// lava's exactly; blob motion integrates a load-scaled rate per frame so
// speed changes glide instead of jumping.
//
// config:
//   smoothing_seconds  load smoothing time constant (default 0.7)
//   palette            cool -> hot stops; each side's window slides along
//                      it with that side's load (default
//                      "00e0c0,2060ff,a040ff,ff4060,ff9000")
//   blobs              blobs per side (default 2)
//   radius             blob size as a fraction of the half-strip
//                      (default 0.3)
//   speed              overall motion multiplier (default 1.0)
//   boil               vigor at full load as a multiple of a nominal pace
//                      (default 2.5); vigor_idle sets the idle end
//   vigor_idle         vigor at zero load (default 0.25)
//   color_span         how much of the palette a blob's edge->core ramp
//                      spans, 0..1 (default 0.45); the rest is the travel
//                      the window slides with load
//   core               field value mapping to the window's hot end / full
//                      brightness (default 1.4)
//   white_hot          overlap bloom strength, 0 disables (default 2.0)
//   min_brightness     floor 0..1 for the dark liquid (default 0.0)
class LoadBoil : public Effect
{
public:
    void init(const EffectConfig& cfg, int) override
    {
        setFrameDelay(cfg, 16);

        smoothing = cfg.getFloat("smoothing_seconds", 0.7f);
        palette = color::Gradient(cfg.get(
            "palette", "00e0c0,2060ff,a040ff,ff4060,ff9000"));
        blobs = cfg.getInt("blobs", 2);
        if (blobs < 1) blobs = 1;
        if (blobs > kMaxPerSide) blobs = kMaxPerSide;
        radius = cfg.getFloat("radius", 0.3f);
        if (radius < 0.02f) radius = 0.02f;
        speed = cfg.getFloat("speed", 1.0f);
        boil = cfg.getFloat("boil", 2.5f);
        vigorIdle = cfg.getFloat("vigor_idle", 0.25f);
        if (vigorIdle < 0.0f) vigorIdle = 0.0f;
        colorSpan = cfg.getFloat("color_span", 0.45f);
        if (colorSpan < 0.05f) colorSpan = 0.05f;
        if (colorSpan > 1.0f) colorSpan = 1.0f;
        core = cfg.getFloat("core", 1.4f);
        if (core < 0.1f) core = 0.1f;
        whiteHot = cfg.getFloat("white_hot", 2.0f);
        if (whiteHot < 0.0f) whiteHot = 0.0f;
        minBright = cfg.getFloat("min_brightness", 0.0f);

        // per-blob mutually incommensurate base rates (as lava), seeded at
        // uniform spacing; neighbours start in opposite directions
        for (int s = 0; s < 2; s++)
            for (int k = 0; k < blobs; k++)
            {
                int idx = s * kMaxPerSide + k;
                float g = 0.6180339887f * (idx + 1);
                float frac = g - floorf(g);
                baseRate[s][k] = (0.10f + 0.08f * frac)
                               * ((k & 1) ? -1.0f : 1.0f);
                phase[s][k] = (k + 0.5f) / blobs;
            }

        if (!gpuLoad.available())
            fprintf(stderr, "load_boil: no gpu load source found yet, "
                            "will keep looking\n");

        hwmon::readCpuCounters(prevBusy, prevTotal);
    }

    void render(Strip& strip, float) override
    {
        // motion integrates per-frame phases (so vigor changes glide), so
        // the wall-clock t goes unused
        float dt = frameDelayMs() / 1000.0f;
        float alpha = smoothing > 0 ? 1.0f - expf(-dt / smoothing) : 1.0f;

        cpu += alpha * (readCpuLoad() - cpu);
        gpu += alpha * (readGpuLoad() - gpu);

        // advance each side's blobs by its load-scaled vigor and resolve
        // positions / windows once per frame. Blobs live in u = 0 (outer
        // end) .. 1 (center) of their half, mirrored onto the strip so the
        // two sides' motion is symmetric about the middle.
        float load[2] = { cpu, gpu };
        int n = 0;

        for (int s = 0; s < 2; s++)
        {
            float vigor = vigorIdle + (boil - vigorIdle) * load[s];
            float base = load[s] * (1.0f - colorSpan);

            for (int k = 0; k < blobs; k++)
            {
                phase[s][k] += dt * baseRate[s][k] * vigor * speed;

                float u = motion::pingpong(phase[s][k], 0.15f);
                float breathe = 1.0f
                    + (0.10f + 0.30f * load[s])
                    * sinf(phase[s][k] * 2.4f + 1.1f * (k + 4 * s));

                blob[n].pos = s ? 1.0f - 0.5f * u : 0.5f * u;
                blob[n].inv = 1.0f / (radius * 0.5f * breathe);
                blob[n].base = base;
                n++;
            }
        }

        for (int i = 0; i < strip.size(); i++)
        {
            float x = (float)i / strip.size();

            // lava's additive field: blobs fuse into a hot neck where they
            // meet — including across the center, between the two sides
            float field = 0.0f;
            for (int k = 0; k < n; k++)
            {
                float d = (x - blob[k].pos) * blob[k].inv;
                float g = 1.0f / (1.0f + d * d);
                w[k] = g * g;
                field += w[k];
            }

            float raw = field / core;
            float c = raw > 1.0f ? 1.0f : raw;
            float bloom = (raw - 1.0f) * whiteHot;
            if (bloom < 0.0f) bloom = 0.0f;
            if (bloom > 1.0f) bloom = 1.0f;

            float br = minBright + (1.0f - minBright) * motion::ease(c);

            float fr = 0.0f, fg = 0.0f, fb = 0.0f;
            for (int k = 0; k < n; k++)
            {
                uint8_t r, g, b;
                palette.at(blob[k].base + c * colorSpan, r, g, b);
                fr += w[k] * r;
                fg += w[k] * g;
                fb += w[k] * b;
            }

            float invF = field > 0.0f ? 1.0f / field : 0.0f;
            float cr = fr * invF, cg = fg * invF, cb = fb * invF;
            cr += (255.0f - cr) * bloom;
            cg += (255.0f - cg) * bloom;
            cb += (255.0f - cb) * bloom;

            strip.setPixel(i, (uint8_t)(cr * br), (uint8_t)(cg * br),
                           (uint8_t)(cb * br));
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

    static const int kMaxPerSide = 8;
    struct Blob { float pos, inv, base; };
    Blob blob[kMaxPerSide * 2];
    float w[kMaxPerSide * 2];
    float phase[2][kMaxPerSide];
    float baseRate[2][kMaxPerSide];

    float smoothing = 0.7f;
    color::Gradient palette;
    int blobs = 2;
    float radius = 0.3f;
    float speed = 1.0f;
    float boil = 2.5f;
    float vigorIdle = 0.25f;
    float colorSpan = 0.45f;
    float core = 1.4f;
    float whiteHot = 2.0f;
    float minBright = 0.0f;
    float cpu = 0;
    float gpu = 0;

    hwmon::GpuLoad gpuLoad;
    unsigned long long prevBusy = 0;
    unsigned long long prevTotal = 0;
};

REGISTER_EFFECT("load_boil", LoadBoil)
