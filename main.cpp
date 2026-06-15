#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include <vector>
#include "effect.hpp"
#include "rules.hpp"

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

// one receiver slot ("esp32.power_on" / "esp32.shutdown") as a slot
// string: effect name on the first line, then a `key=value` line per
// setting (see protocol.hpp). Empty when the slot isn't configured, so
// the receiver keeps its built-in default for it
static std::string serializeSlot(const Config& cfg, const std::string& path)
{
    const json::Value* slot = cfg.find(path);
    std::string out;

    if (!slot)
        return out;

    if (const json::Value* e = slot->find("effect"))
        out += json::toString(*e);
    out += '\n';

    if (const json::Value* s = slot->find("settings"))
        if (s->isObject())
            for (auto& m : s->members)
                out += m.first + "=" + json::toString(m.second) + "\n";

    return out;
}

// push the receiver's power-on/shutdown effect config (the "esp32" block)
// so it can store it in NVS and use it for the next power-on and for
// shutdown. Sent once at startup; picked up on the next daemon (re)start.
// Skipped entirely when there's no "esp32" block, so we never clobber a
// remembered config with nothing
static void sendEsp32Config(const Config& cfg, WS2812Serial& strip)
{
    if (!cfg.find("esp32"))
        return;

    std::string po = serializeSlot(cfg, "esp32.power_on");
    std::string sd = serializeSlot(cfg, "esp32.shutdown");

    std::vector<uint8_t> payload;
    payload.insert(payload.end(), po.begin(), po.end());
    payload.push_back(0);
    payload.insert(payload.end(), sd.begin(), sd.end());
    payload.push_back(0);

    strip.sendCommand(proto::CMD_CONFIG, payload.data(),
                      (uint16_t)payload.size());
}

static void usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s <config>           run the rules\n"
            "       %s <config> <effect>  run a single effect\n"
            "       %s --list             list available effects\n",
            prog, prog, prog);
}

int main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0)
    {
        for (auto& name : effectNames())
            printf("%s\n", name.c_str());

        return 0;
    }

    if (argc != 2 && argc != 3)
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

    WS2812Serial strip = WS2812Serial::fromConfig(cfg);

    // tell the receiver which effects to run at power-on and shutdown
    sendEsp32Config(cfg, strip);

    std::unique_ptr<Effect> effect;
    std::string active;
    const json::Value* activeSettings = nullptr;
    double effectStart = 0;

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
        strip.beginFrame();
        effect->render(strip, (float)(now_seconds() - effectStart));

        // exit on write failure so systemd restarts us and reopens the port
        if (!strip.show())
        {
            fprintf(stderr, "serial write failed, exiting\n");
            return false;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(effect->frameDelayMs()));

        return true;
    };

    // on shutdown, hand off to the receiver: it plays the shutdown
    // effect to completion on its own clock, so the daemon can exit
    // immediately instead of blocking systemd while it fades. The strip
    // is independently powered, so it outlives us. (Old firmware that
    // doesn't know the command ignores it and blanks on its host timeout.)
    auto notifyShutdown = [&]()
    {
        strip.sendCommand(proto::CMD_SHUTDOWN);
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

    // the boot animation is the receiver's power-on effect (config's
    // "esp32" block), played at true power-on while the OS comes up — so
    // the daemon goes straight to the rules instead of replaying it here

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
