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
    float settle; // "for": seconds the condition must have held before the
                  // rule matches; 0 = match on the first true reading

    // the condition, debounced by "for": the reading and when it last
    // flipped, and whether the rule currently matches
    bool reading = false;
    double since = -1e9;
    bool settled = false;

    // evaluate the condition at `now` and say whether the rule matches.
    // With "for" set, a true reading only counts once it has held that
    // long (a spike doesn't start the rule); a false one drops it at once
    bool matches(double now);

    // a "for" rule keeps its own clock, so it wants evaluating every tick
    // even while a rule above it is winning — otherwise its timer would
    // start over the moment the higher rule let go
    bool watched() const { return settle > 0; }
};

// "rules" is an array of { "if": <condition>, "effect": <name>,
// "settings": { ... }, "hold": <seconds>, "for": <seconds> }; "if"
// defaults to always, so an entry without one is a catch-all. "hold" is
// the min seconds since the last switch before this rule may take over;
// "for" is how long its condition must have held before the rule matches
// (an on-delay: a load spike shorter than that never starts the rule).
// Both default to 0
bool loadRules(const Config& cfg, std::vector<Rule>& rules);

// carry the "for" clocks of `from` over to `to`, rules being identical
// (a reload that kept the effect): without it every settled rule would
// start its wait over
void carryTimers(const std::vector<Rule>& from, std::vector<Rule>& to);

// equal when both are absent or hold the same values, so hopping
// between rules that configure an effect identically doesn't restart it
bool sameSettings(const json::Value* a, const json::Value* b);
