#pragma once

#include <memory>
#include <string>
#include <vector>
#include "condition.hpp"
#include "config_loader.hpp"

struct Rule
{
    std::unique_ptr<Condition> condition;
    std::string effect;
    const json::Value* settings; // points into the Config tree; may be null
    float hold; // min seconds since the last switch; defaults to 0
};

// "rules" is an array of { "if": <condition>, "effect": <name>,
// "settings": { ... }, "hold": <seconds> }; "if" defaults to always,
// so an entry without one is a catch-all. "hold" is the min seconds
// since the last switch before this rule may take over (debounces
// flapping conditions); it defaults to 0, so a rule that wants to
// avoid strobing sets its own
bool loadRules(const Config& cfg, std::vector<Rule>& rules);

// equal when both are absent or hold the same values, so hopping
// between rules that configure an effect identically doesn't restart it
bool sameSettings(const json::Value* a, const json::Value* b);
