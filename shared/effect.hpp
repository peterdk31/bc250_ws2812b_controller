#pragma once

#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <stdint.h>
#include <stdlib.h>

// Effects render against `Strip`, a thin pixel sink (setPixel/size), and read
// tuning through `EffectConfig` (the triggering rule's settings, over JSON).
// This is host-only: the ESP32 receiver renders no effects — it replays
// recordings the daemon streams (see host/recorder.hpp, shared/protocol.hpp).

// "RRGGBB" or "#RRGGBB" -> 0xRRGGBB; returns def for an empty string.
inline uint32_t parseHexColor(const std::string& v, uint32_t def)
{
    if (v.empty())
        return def;

    const char* s = v.c_str() + (v[0] == '#' ? 1 : 0);
    return (uint32_t)strtoul(s, nullptr, 16);
}

#include "config_loader.hpp"
#include "strip.hpp"

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

    bool getBool(const std::string& key, bool def = false) const
    {
        const json::Value* v = find(key);
        return v ? json::toBool(*v, def) : def;
    }

    uint32_t getColor(const std::string& key, uint32_t def) const
    {
        return parseHexColor(get(key), def);
    }

    // escape hatch for effects that compose others (e.g. `cycle`): the
    // resolved json value for a key (overrides first, then top-level),
    // and the Config to spin up child views from. Host-only — the
    // receiver's EffectConfig has no json behind it.
    const json::Value* raw(const std::string& key) const { return find(key); }
    const Config& config() const { return cfg; }

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
    virtual void render(Strip& strip, float t) = 0;

    // an effect may declare itself complete; the receiver uses this to
    // end a one-shot power-on effect (handing off to the idle) and to
    // stop the shutdown effect
    virtual bool finished() const { return false; }

    // delay between frames. Returns the value resolved by setFrameDelay()
    // (an effect's own default, overridable by a `frame_ms` setting); cycle
    // overrides this to defer to whichever sub-effect it's currently running.
    virtual int frameDelayMs() const { return frameMs_; }

protected:
    // resolve the per-frame delay in init(): a `frame_ms` setting wins if
    // present — rule settings first, then top-level config as a global
    // default — otherwise the effect's own default `def`. Effects that step
    // state per frame read frameDelayMs() as their timestep, so the one value
    // drives both the refresh rate and dt: lowering it smooths motion without
    // changing speed. Floored at 1ms so the render loop can't busy-spin.
    void setFrameDelay(const EffectConfig& cfg, int def)
    {
        int ms = cfg.getInt("frame_ms", def);
        frameMs_ = ms < 1 ? 1 : ms;
    }

    int frameMs_ = 16;
};

using EffectFactory = std::unique_ptr<Effect> (*)();

bool registerEffect(const char* name, EffectFactory factory);
std::unique_ptr<Effect> createEffect(const std::string& name);
std::vector<std::string> effectNames();

// place once in each effect source file
#define REGISTER_EFFECT(name, Class) \
    static const bool reg_##Class = registerEffect(name, \
        []() -> std::unique_ptr<Effect> { return std::make_unique<Class>(); });
