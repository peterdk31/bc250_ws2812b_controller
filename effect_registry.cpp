#include "effect.hpp"

#include <map>

// function-local static so registration from any translation unit's
// static initializers is safe regardless of init order
static std::map<std::string, EffectFactory>& registry()
{
    static std::map<std::string, EffectFactory> r;
    return r;
}

bool registerEffect(const char* name, EffectFactory factory)
{
    registry()[name] = factory;
    return true;
}

std::unique_ptr<Effect> createEffect(const std::string& name)
{
    auto it = registry().find(name);
    if (it == registry().end())
        return nullptr;

    return it->second();
}

std::vector<std::string> effectNames()
{
    std::vector<std::string> names;

    for (auto& kv : registry())
        names.push_back(kv.first);

    return names;
}
