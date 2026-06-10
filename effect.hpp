#pragma once

#include <memory>
#include <string>
#include <vector>
#include "config_loader.hpp"
#include "ws2812_serial.hpp"

// config view for one effect activation: the triggering rule's
// "settings" object, falling back to top-level keys for config shared
// across effects (e.g. `sensors`); everything else is the effect's
// built-in default
class EffectConfig
{
public:
    EffectConfig(const Config& cfg, const json::Value* overrides = nullptr)
        : cfg(cfg), overrides(overrides) {}

    std::string get(const std::string& key, const std::string& def = "") const
    {
        const json::Value* v = find(key);
        return v ? json::toString(*v, def) : def;
    }

    int getInt(const std::string& key, int def = 0) const
    {
        const json::Value* v = find(key);
        return v ? json::toInt(*v, def) : def;
    }

    float getFloat(const std::string& key, float def = 0.0f) const
    {
        const json::Value* v = find(key);
        return v ? json::toFloat(*v, def) : def;
    }

    // "RRGGBB" or "#RRGGBB" as 0xRRGGBB
    uint32_t getColor(const std::string& key, uint32_t def) const
    {
        auto v = get(key);
        if (v.empty())
            return def;

        if (v[0] == '#')
            v.erase(0, 1);

        return (uint32_t)strtoul(v.c_str(), nullptr, 16);
    }

private:
    const json::Value* find(const std::string& key) const
    {
        if (overrides)
            if (const json::Value* v = overrides->find(key))
                return v;

        return cfg.root().find(key);
    }

    const Config& cfg;
    const json::Value* overrides;
};

class Effect
{
public:
    virtual ~Effect() = default;

    // called once when the effect becomes active
    virtual void init(const EffectConfig& cfg, int leds)
    {
        (void)cfg;
        (void)leds;
    }

    // fill the current frame; t is seconds since this effect became active
    virtual void render(WS2812Serial& strip, float t) = 0;

    // an effect may declare itself complete; the daemon uses this to
    // end the startup one-shot before the rule engine takes over
    virtual bool finished() const { return false; }

    // delay between frames
    virtual int frameDelayMs() const { return 16; }
};

using EffectFactory = std::unique_ptr<Effect> (*)();

bool registerEffect(const char* name, EffectFactory factory);
std::unique_ptr<Effect> createEffect(const std::string& name);
std::vector<std::string> effectNames();

// place once in each effect source file
#define REGISTER_EFFECT(name, Class) \
    static const bool reg_##Class = registerEffect(name, \
        []() -> std::unique_ptr<Effect> { return std::make_unique<Class>(); });
