#include <stdio.h>
#include <string.h>
#include <time.h>
#include <thread>
#include <chrono>
#include <vector>
#include "effect.hpp"
#include "rules.hpp"

static double now_seconds()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(const char* prog)
{
    fprintf(stderr,
            "Usage: %s <config>           run startup effect, then rules\n"
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

    WS2812Serial strip = WS2812Serial::fromConfig(cfg);

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

    // single-effect mode, no rules
    if (argc == 3)
    {
        if (!activate(argv[2], nullptr))
            return 1;

        while (true)
            if (!renderFrame())
                return 1;
    }

    std::vector<Rule> rules;

    if (!loadRules(cfg, rules))
        return 1;

    // startup one-shot, until the effect reports finished; same shape
    // as a rule but without "if", omit it for no startup effect.
    // "max_seconds" caps it so an effect that never finishes (e.g.
    // rainbow) can't wedge the daemon in startup forever
    if (const json::Value* startup = cfg.root().find("startup"))
    {
        const json::Value* e = startup->find("effect");

        if (!e || !e->isString())
        {
            fprintf(stderr, "startup: expected an object with \"effect\"\n");
            return 1;
        }

        const json::Value* cap = startup->find("max_seconds");
        float maxSeconds = cap ? json::toFloat(*cap, 30.0f) : 30.0f;

        if (!activate(e->text, startup->find("settings")))
            return 1;

        while (!effect->finished()
               && now_seconds() - effectStart < maxSeconds)
            if (!renderFrame())
                return 1;
    }

    float holdSeconds = cfg.getFloat("hold_seconds", 3.0f);
    const double evalInterval = 0.5;

    double lastEval = -1e9;
    double lastSwitch = -1e9;
    const Rule* want = nullptr;

    while (true)
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

        float hold = want && want->hold >= 0 ? want->hold : holdSeconds;

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
            // no startup effect and no matching rule yet
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}
