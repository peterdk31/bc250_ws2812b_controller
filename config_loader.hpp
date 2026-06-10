#pragma once

#include <stdio.h>
#include <fstream>
#include <sstream>
#include <string>
#include "json.hpp"

class Config
{
public:
    bool load(const std::string& path)
    {
        std::ifstream file(path);

        if (!file.is_open())
            return false;

        std::stringstream buf;
        buf << file.rdbuf();

        std::string err;

        if (!json::parse(buf.str(), rootValue, err))
        {
            fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            return false;
        }

        if (!rootValue.isObject())
        {
            fprintf(stderr, "%s: top level must be an object\n", path.c_str());
            return false;
        }

        return true;
    }

    const json::Value& root() const { return rootValue; }

    // dotted path into nested objects, e.g. "serial.port"
    const json::Value* find(const std::string& path) const
    {
        const json::Value* v = &rootValue;
        size_t start = 0;

        while (v)
        {
            size_t dot = path.find('.', start);

            if (dot == std::string::npos)
                return v->find(path.substr(start));

            v = v->find(path.substr(start, dot - start));
            start = dot + 1;
        }

        return nullptr;
    }

    std::string get(const std::string& path, const std::string& def = "") const
    {
        const json::Value* v = find(path);
        return v ? json::toString(*v, def) : def;
    }

    int getInt(const std::string& path, int def = 0) const
    {
        const json::Value* v = find(path);
        return v ? json::toInt(*v, def) : def;
    }

    float getFloat(const std::string& path, float def = 0.0f) const
    {
        const json::Value* v = find(path);
        return v ? json::toFloat(*v, def) : def;
    }

private:
    json::Value rootValue;
};
