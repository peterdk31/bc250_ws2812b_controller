#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string>
#include <vector>
#include "effect.hpp"

// idle "channel surfer": runs a list of other effects one at a time,
// hopping to a random next one every period_seconds (or a per-entry
// override). Each sub-effect gets its own settings block and its own
// clock — exactly as if a rule had selected it directly — and top-level
// keys (e.g. sensors) still fall through. Host-only: it leans on the
// JSON config to carry a nested list, and the receiver has no parser.
//
// config:
//   period_seconds  default seconds to show each effect (default 30)
//   effects         array of:
//                     { "effect": name,
//                       "settings": { ... },   (optional, per-effect)
//                       "seconds": n }          (optional, overrides period)
//
// a finite sub-effect (one whose finished() goes true, e.g. a one-shot
// bloom) hands off early rather than freezing on its last frame. The
// cycle itself never finishes, so it's a drop-in for the idle rule.
class Cycle : public Effect
{
public:
    void init(const EffectConfig& cfg, int leds) override
    {
        // seed rand() once (shared with twinkle/fire/comet) so the
        // cycle's order — and those effects' flicker — differ per boot
        // instead of replaying the same sequence every restart
        static bool seeded = (srand((unsigned)time(nullptr)), true);
        (void)seeded;

        ledCount = leds;
        config = &cfg.config();

        defaultPeriod = cfg.getFloat("period_seconds", 30.0f);
        if (defaultPeriod <= 0) defaultPeriod = 30.0f;

        entries.clear();

        const json::Value* list = cfg.raw("effects");

        if (list && list->isArray())
        {
            for (auto& item : list->items)
            {
                const json::Value* name = item.find("effect");
                if (!name)
                    continue;

                Entry e;
                e.name = json::toString(*name);

                // skip unknown effects up front so a typo doesn't park
                // the strip on a blank frame for a whole period
                if (!createEffect(e.name))
                {
                    fprintf(stderr, "cycle: unknown effect '%s', skipping\n",
                            e.name.c_str());
                    continue;
                }

                e.settings = item.find("settings");

                const json::Value* secs = item.find("seconds");
                e.seconds = secs ? json::toFloat(*secs, defaultPeriod)
                                 : defaultPeriod;
                if (e.seconds <= 0) e.seconds = defaultPeriod;

                entries.push_back(e);
            }
        }

        if (entries.empty())
        {
            fprintf(stderr, "cycle: no effects configured\n");
            return;
        }

        current = (int)(rand() % entries.size());
        start(current, 0.0f);
    }

    void render(Strip& strip, float t) override
    {
        if (!sub)
            return;

        float localT = t - subStart;

        if (localT >= entries[current].seconds || sub->finished())
        {
            advance(t);
            localT = 0.0f;
        }

        sub->render(strip, localT);
    }

    // the idle never ends on its own
    bool finished() const override { return false; }

    int frameDelayMs() const override
    {
        return sub ? sub->frameDelayMs() : 33;
    }

private:
    struct Entry
    {
        std::string name;
        const json::Value* settings = nullptr;
        float seconds = 30.0f;
    };

    // build a fresh sub-effect, resolving its settings against the same
    // Config the cycle was given so it falls back to top-level keys just
    // like a top-level rule would
    void start(int index, float t)
    {
        current = index;
        sub = createEffect(entries[current].name);
        sub->init(EffectConfig(*config, entries[current].settings), ledCount);
        subStart = t;
    }

    // pick a different entry at random (or the only one), then start it
    void advance(float t)
    {
        int next = current;

        if (entries.size() > 1)
            while (next == current)
                next = (int)(rand() % entries.size());

        start(next, t);
    }

    std::vector<Entry> entries;
    const Config* config = nullptr;
    std::unique_ptr<Effect> sub;
    int current = 0;
    int ledCount = 0;
    float defaultPeriod = 30.0f;
    float subStart = 0.0f;
};

REGISTER_EFFECT("cycle", Cycle)
