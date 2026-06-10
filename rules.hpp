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
    float hold; // min seconds since the last switch; <0 = global default
};

// "rules" is an array of { "if": <condition>, "effect": <name>,
// "settings": { ... }, "hold": <seconds> }; "if" defaults to always,
// so an entry without one is a catch-all. "hold" overrides the global
// hold_seconds — 0 lets urgent rules (e.g. the overheat alarm) take
// over immediately
bool loadRules(const Config& cfg, std::vector<Rule>& rules);

// equal when both are absent or hold the same values, so hopping
// between rules that configure an effect identically doesn't restart it
bool sameSettings(const json::Value* a, const json::Value* b);
