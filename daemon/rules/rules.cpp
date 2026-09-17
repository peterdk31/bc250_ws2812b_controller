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
        const json::Value* settle = entry.find("for");

        Rule rule;
        rule.condition = std::move(condition);
        rule.effect = effect->text;
        rule.settings = settings;
        rule.hold = hold ? json::toFloat(*hold, 0.0f) : 0.0f;
        rule.settle = settle ? json::toFloat(*settle, 0.0f) : 0.0f;
        rules.push_back(std::move(rule));
    }

    // no implicit default: with no rules configured nothing matches, so
    // the daemon simply leaves the strip dark until one does

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

bool Rule::matches(double now)
{
    bool raw = condition->eval();

    if (settle <= 0)
        return raw;

    // on-delay only: a false reading drops the rule at once, a true one
    // has to hold for "for" seconds first (a fresh rule starts false, so a
    // condition true at startup waits like any other rise)
    if (raw != reading)
    {
        reading = raw;
        since = now;
    }

    settled = reading && now - since >= settle;
    return settled;
}

void carryTimers(const std::vector<Rule>& from, std::vector<Rule>& to)
{
    if (from.size() != to.size())
        return;

    for (size_t i = 0; i < to.size(); i++)
    {
        to[i].reading = from[i].reading;
        to[i].since = from[i].since;
        to[i].settled = from[i].settled;
    }
}

bool sameSettings(const json::Value* a, const json::Value* b)
{
    if (a == b)
        return true;

    if (!a || !b)
        return (a ? a : b)->members.empty();

    return json::equal(*a, *b);
}
