#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>
#include "effect.hpp"
#include "motion.hpp"
#include "rules.hpp"
#include "steam.hpp"
#include "serial_sink.hpp"
#include "virtual_sink.hpp"
#include "recorder.hpp"

// set from a signal handler; the render loops watch it so a SIGTERM
// from systemd (or Ctrl-C) breaks out and lets us tell the receiver to
// play the shutdown effect before we exit
static volatile sig_atomic_t g_stop = 0;
static void onStop(int) { g_stop = 1; }

static double now_seconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// wrap already-corrected recording pixels into a wire frame (--preview): the
// bytes are post-LUT, so unlike Strip they must not be re-corrected. Checksum
// is XOR of the header and pixel bytes, matching common/protocol.hpp. A
// recording is one continuous animation (its own intro/loop is baked in), so
// every frame carries the same anim id (0) and no crossfade (xms 0) — the
// viewer plays it straight through with no dissolves.
static std::vector<uint8_t> framePixels(uint8_t pin, uint16_t count,
                                        const uint8_t* px)
{
    std::vector<uint8_t> f;
    f.reserve(proto::PIX_HEADER + (size_t)count * 3 + 1);

    f.push_back(proto::SYNC0);
    f.push_back(proto::SYNC1);
    f.push_back(pin);
    f.push_back(count & 0xFF);
    f.push_back(count >> 8);
    f.push_back(0); // anim lo
    f.push_back(0); // anim hi
    f.push_back(0); // xms lo
    f.push_back(0); // xms hi

    // the four zero header bytes XOR out, so the sum is pin/count/pixels
    uint8_t sum = pin ^ (uint8_t)(count & 0xFF) ^ (uint8_t)(count >> 8);

    for (uint16_t i = 0; i < count * 3; i++)
    {
        f.push_back(px[i]);
        sum ^= px[i];
    }

    f.push_back(sum);
    return f;
}

// render the receiver's power-on/shutdown effects (the "esp32" block) to
// frames and stream them over so the receiver can store and replay them — at
// the next power-on (before the daemon is up) and on shutdown (after it
// exits). The receiver runs no effect code of its own; these recordings are
// the only thing it plays standalone. Done once at startup, so an edited
// effect is picked up on the next daemon (re)start and shown one power cycle
// later. Skipped entirely when there's no "esp32" block. The recording uses
// `strip` purely as a correction canvas — by value inside record(), so the
// daemon's own Strip is untouched. Broadcast to every sink; only the serial
// transport acts on commands, the rest no-op (see host/sink.hpp).
static void recordAndUpload(const Config& cfg, const Strip& strip,
                            std::vector<std::unique_ptr<Sink>>& sinks)
{
    if (!cfg.find("esp32"))
        return;

    const struct { const char* path; uint8_t id; } slots[] = {
        {"esp32.power_on", proto::SLOT_POWER_ON},
        {"esp32.shutdown", proto::SLOT_SHUTDOWN},
    };

    for (auto& slot : slots)
    {
        rec::Recording r;

        if (rec::record(cfg, strip, slot.path, r))
            rec::upload(sinks, slot.id, r);
    }
}

// ./led <config> --preview <slot>: record an esp32 slot and play it back to
// the sinks exactly as the receiver will, so the viewer previews the real
// recording — its one-shot intro, then the looping tail (or a held last frame)
// — not just the live effect. `slotArg` is "power_on"/"shutdown" or a dotted
// config path. Runs until interrupted; returns the process exit code.
static int runPreview(const Config& cfg, Strip& strip,
                      std::vector<std::unique_ptr<Sink>>& sinks,
                      const std::string& slotArg)
{
    std::string slot = slotArg;
    if (slot.find('.') == std::string::npos)
        slot = "esp32." + slot;

    rec::Recording r;

    if (!rec::record(cfg, strip, slot, r) || !r.valid())
    {
        fprintf(stderr, "preview: could not record %s\n", slot.c_str());
        return 1;
    }

    fprintf(stderr,
            "preview %s: %u frames @ %u ms, loop=%d loopStart=%u "
            "(Ctrl-C to stop)\n",
            slot.c_str(), r.frameCount, r.frameMs, r.loop, r.loopStart);

    // drive the shared Player exactly as the receiver does
    rec::Player player;
    player.start(&r);

    double start = now_seconds();
    int sleepMs = r.frameMs / 2 < 1 ? 1 : r.frameMs / 2;

    while (!g_stop)
    {
        uint32_t nowMs = (uint32_t)((now_seconds() - start) * 1000.0);

        if (const uint8_t* px = player.tick(nowMs))
        {
            std::vector<uint8_t> frame = framePixels(r.pin, r.count, px);
            for (auto& s : sinks)
                s->send(frame);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    return 0;
}

static void usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s <config>                       run the rules\n"
            "       %s <config> <effect>              run a single effect\n"
            "       %s <config> --preview <slot>      replay a recorded esp32 slot\n"
            "                                         (power_on/shutdown) to the viewer\n"
            "       %s --list                         list available effects\n"
            "       %s --steam-status                 dump Steam download detection\n"
            "       %s --config-get <config> <path>   print a config value\n",
            prog, prog, prog, prog, prog, prog);
}

int main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0)
    {
        for (auto& name : effectNames())
            printf("%s\n", name.c_str());

        return 0;
    }

    // one verbose Steam scan, then exit: prints which libraries and
    // appmanifests the download detector sees and why each does or
    // doesn't count, so a blank steam_download bar can be traced to its
    // cause (no library matched, manifest paused/not-running, etc.)
    if (argc == 2 && strcmp(argv[1], "--steam-status") == 0)
    {
        steam::dumpStatus(stdout);
        return 0;
    }

    // read one value out of the config through the daemon's own parser,
    // so the build (Makefile) can't drift from how the daemon reads the
    // same file. prints the scalar at <path> (dotted, e.g. "sinks.serial.port")
    // and exits 0; exits 1 with no output when the key is absent, so the
    // caller can fall back to its own default
    if (argc == 4 && strcmp(argv[1], "--config-get") == 0)
    {
        Config cfg;

        if (!cfg.load(argv[2]))
            return 1;

        const json::Value* v = cfg.find(argv[3]);

        if (!v)
            return 1;

        printf("%s\n", json::toString(*v).c_str());
        return 0;
    }

    // ./led <config> --preview <slot>: record an esp32 slot and play it back to
    // the sinks, exactly as the receiver will — so the viewer previews the real
    // recording (sequence, loop and hold included), not just the live effect
    bool previewMode = (argc == 4 && strcmp(argv[2], "--preview") == 0);

    if (!previewMode && argc != 2 && argc != 3)
    {
        usage(argv[0]);
        return 1;
    }

    Config cfg;

    if (!cfg.load(argv[1]))
    {
        fprintf(stderr, "failed to load config: %s\n", argv[1]);
        return 1;
    }

    signal(SIGTERM, onStop);
    signal(SIGINT, onStop);

    Strip strip = Strip::fromConfig(cfg);

    // outputs the rendered frame goes to. The list is the fan-out: a serial
    // transport for the real strip (absent when running headless), plus the
    // on-screen viewer mirror (always attached — it's a no-op until a viewer
    // binds its socket). An empty list is fine — the daemon just renders to
    // nothing. (see host/sink.hpp)
    std::vector<std::unique_ptr<Sink>> sinks;

    if (auto serial = SerialSink::fromConfig(cfg))
        sinks.push_back(std::move(serial));

    if (auto viewer = VirtualSink::create())
        sinks.push_back(std::move(viewer));

    if (previewMode)
        return runPreview(cfg, strip, sinks, argv[3]);

    // record the power-on/shutdown effects and stream them for the receiver
    // to replay (the receiver renders nothing itself)
    recordAndUpload(cfg, strip, sinks);

    std::unique_ptr<Effect> effect;
    std::string active;
    const json::Value* activeSettings = nullptr;
    double effectStart = 0;

    // crossfading now lives on the receiver (and the viewer): every frame
    // carries the rendering effect's id and a global crossfade duration, and
    // they dissolve whenever the id changes (see common/fade.hpp). The daemon
    // just stamps those — it composites nothing itself. The same duration
    // drives the live→shutdown dissolve (sent with CMD_SHUTDOWN).
    const uint16_t crossfadeMs = (uint16_t)cfg.getInt("crossfade_ms", 600);
    strip.setTransitionMs(crossfadeMs);

    auto activate = [&](const std::string& name,
                        const json::Value* settings) -> bool
    {
        effect = createEffect(name);

        if (!effect)
        {
            fprintf(stderr, "unknown effect: %s\n", name.c_str());
            return false;
        }

        effect->init(EffectConfig(cfg, settings), strip.size());
        active = name;
        activeSettings = settings;
        effectStart = now_seconds();
        return true;
    };

    auto renderFrame = [&]() -> bool
    {
        double now = now_seconds();

        strip.beginFrame();
        effect->render(strip, (float)(now - effectStart));

        // stamp the id of whatever actually rendered — a composite like cycle
        // reports its active child, so its internal hops change the id and the
        // receiver crossfades them like any rule-level switch. Done after
        // render() (cycle may have advanced) and before endFrame().
        strip.setAnimId((uint16_t)effect->currentId());

        // hand the finished wire frame to every sink.
        // A fatal sink (serial write failure) ends the loop so systemd
        // restarts us and reopens the port; best-effort sinks (the viewer)
        // never report one.
        const std::vector<uint8_t>& frame = strip.endFrame();

        for (auto& s : sinks)
        {
            if (!s->send(frame))
            {
                fprintf(stderr, "sink send failed, exiting\n");
                return false;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(effect->frameDelayMs()));

        return true;
    };

    // on shutdown, hand off to the receiver: it replays the stored shutdown
    // recording to completion on its own clock, so the daemon can exit
    // immediately instead of blocking systemd while it fades. The strip
    // is independently powered, so it outlives us. (A receiver with no
    // shutdown recording yet just blanks on its host timeout.)
    auto notifyShutdown = [&]()
    {
        // carry the crossfade duration so the receiver dissolves from the last
        // live frame into the shutdown recording instead of snapping
        const uint8_t dur[2] = {(uint8_t)(crossfadeMs & 0xFF),
                                (uint8_t)(crossfadeMs >> 8)};

        for (auto& s : sinks)
            s->sendCommand(proto::CMD_SHUTDOWN, dur, 2);
    };

    // single-effect mode, no rules
    if (argc == 3)
    {
        if (!activate(argv[2], nullptr))
            return 1;

        while (!g_stop)
            if (!renderFrame())
                return 1;

        notifyShutdown();
        return 0;
    }

    std::vector<Rule> rules;

    if (!loadRules(cfg, rules))
        return 1;

    // the boot animation is the receiver replaying its stored power-on
    // recording (config's "esp32" block) at true power-on while the OS comes
    // up — so the daemon goes straight to the rules instead of playing it here

    const double evalInterval = 0.5;

    double lastEval = -1e9;
    double lastSwitch = -1e9;
    const Rule* want = nullptr;

    while (!g_stop)
    {
        double now = now_seconds();

        if (now - lastEval >= evalInterval)
        {
            lastEval = now;

            for (auto& r : rules)
            {
                if (r.condition->eval())
                {
                    want = &r;
                    break;
                }
            }
        }

        bool changed = want &&
            (want->effect != active ||
             !sameSettings(want->settings, activeSettings));

        float hold = want ? want->hold : 0.0f;

        if (changed && now - lastSwitch >= hold)
        {
            if (!activate(want->effect, want->settings))
                return 1;

            lastSwitch = now;
        }

        if (effect)
        {
            if (!renderFrame())
                return 1;
        }
        else
        {
            // nothing matched yet, so no effect is active to render
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    notifyShutdown();
    return 0;
}
