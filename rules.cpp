#include "rules.hpp"

#include <stdio.h>
#include "effect.hpp"

bool loadRules(const Config& cfg, std::vector<Rule>& rules)
{
    const json::Value* list = cfg.root().find("rules");

    if (list && !list->isArray())
    {
        fprintf(stderr, "rules: expected an array\n");
        return false;
    }

    for (size_t i = 0; list && i < list->items.size(); i++)
    {
        const json::Value& entry = list->items[i];
        std::string where = "rules[" + std::to_string(i) + "]";

        const json::Value* effect = entry.find("effect");

        if (!effect || !effect->isString())
        {
            fprintf(stderr, "%s: expected an object with \"effect\"\n",
                    where.c_str());
            return false;
        }

        const json::Value* cond = entry.find("if");
        std::string condSpec = cond ? json::toString(*cond) : "always";

        auto condition = parseCondition(condSpec, cfg);

        if (!condition)
        {
            fprintf(stderr, "%s: unknown condition '%s'\n",
                    where.c_str(), condSpec.c_str());
            return false;
        }

        const json::Value* settings = entry.find("settings");

        if (settings && !settings->isObject())
        {
            fprintf(stderr, "%s: \"settings\" must be an object\n",
                    where.c_str());
            return false;
        }

        const json::Value* hold = entry.find("hold");

        rules.push_back({std::move(condition), effect->text, settings,
                         hold ? json::toFloat(*hold, -1.0f) : -1.0f});
    }

    if (rules.empty())
        rules.push_back({parseCondition("always", cfg), "cpu_temp", nullptr,
                         -1.0f});

    // fail at startup, not mid-run, on a typo'd effect name
    for (auto& r : rules)
    {
        if (!createEffect(r.effect))
        {
            fprintf(stderr, "rules reference unknown effect '%s'\n",
                    r.effect.c_str());
            return false;
        }
    }

    return true;
}

bool sameSettings(const json::Value* a, const json::Value* b)
{
    if (a == b)
        return true;

    if (!a || !b)
        return (a ? a : b)->members.empty();

    return json::equal(*a, *b);
}
