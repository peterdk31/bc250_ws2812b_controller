#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

// minimal strict JSON parser
namespace json
{

class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;

    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }

    // object member by key; nullptr when absent or not an object
    const Value* find(const std::string& key) const
    {
        if (type != Type::Object)
            return nullptr;

        for (auto& m : members)
            if (m.first == key)
                return &m.second;

        return nullptr;
    }
};

// scalar as text; arrays of scalars join with ',' so candidate lists
// (e.g. sensors) read the same as the equivalent single string
inline std::string toString(const Value& v, const std::string& def = "")
{
    switch (v.type)
    {
        case Value::Type::String:
            return v.text;

        case Value::Type::Bool:
            return v.boolean ? "true" : "false";

        case Value::Type::Number:
        {
            char buf[32];
            snprintf(buf, sizeof buf, "%g", v.number);
            return buf;
        }

        case Value::Type::Array:
        {
            std::string joined;

            for (auto& item : v.items)
            {
                if (!joined.empty())
                    joined += ',';

                joined += toString(item);
            }

            return joined;
        }

        default:
            return def;
    }
}

inline int toInt(const Value& v, int def = 0)
{
    if (v.type == Value::Type::Number) return (int)v.number;
    if (v.type == Value::Type::String) return atoi(v.text.c_str());
    return def;
}

inline float toFloat(const Value& v, float def = 0.0f)
{
    if (v.type == Value::Type::Number) return (float)v.number;
    if (v.type == Value::Type::String) return (float)atof(v.text.c_str());
    return def;
}

// deep equality; object member order doesn't matter
inline bool equal(const Value& a, const Value& b)
{
    if (a.type != b.type)
        return false;

    switch (a.type)
    {
        case Value::Type::Null:   return true;
        case Value::Type::Bool:   return a.boolean == b.boolean;
        case Value::Type::Number: return a.number == b.number;
        case Value::Type::String: return a.text == b.text;

        case Value::Type::Array:
            if (a.items.size() != b.items.size())
                return false;

            for (size_t i = 0; i < a.items.size(); i++)
                if (!equal(a.items[i], b.items[i]))
                    return false;

            return true;

        case Value::Type::Object:
            if (a.members.size() != b.members.size())
                return false;

            for (auto& m : a.members)
            {
                const Value* v = b.find(m.first);

                if (!v || !equal(m.second, *v))
                    return false;
            }

            return true;
    }

    return false;
}

class Parser
{
public:
    explicit Parser(const std::string& text) : s(text) {}

    bool parse(Value& out, std::string& err)
    {
        skip();

        bool ok = parseValue(out);

        if (ok)
        {
            skip();

            if (pos < s.size())
                ok = fail("unexpected trailing content");
        }

        if (!ok)
            err = "line " + std::to_string(line()) + ": " + message;

        return ok;
    }

private:
    const std::string& s;
    size_t pos = 0;
    size_t errPos = 0;
    std::string message;

    // record the first failure only, so the message points at the
    // innermost problem rather than its enclosing containers
    bool fail(const char* m)
    {
        if (message.empty())
        {
            message = m;
            errPos = pos;
        }

        return false;
    }

    int line() const
    {
        int n = 1;

        for (size_t i = 0; i < errPos && i < s.size(); i++)
            if (s[i] == '\n')
                n++;

        return n;
    }

    void skip()
    {
        while (pos < s.size())
        {
            char c = s[pos];

            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                break;

            pos++;
        }
    }

    bool literal(const char* word)
    {
        size_t len = strlen(word);

        if (s.compare(pos, len, word) != 0)
            return false;

        pos += len;
        return true;
    }

    bool parseValue(Value& out)
    {
        if (pos >= s.size())
            return fail("unexpected end of input");

        char c = s[pos];

        if (c == '{')
            return parseObject(out);

        if (c == '[')
            return parseArray(out);

        if (c == '"')
        {
            out.type = Value::Type::String;
            return parseString(out.text);
        }

        if (c == '-' || (c >= '0' && c <= '9'))
            return parseNumber(out);

        if (literal("true"))
        {
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }

        if (literal("false"))
        {
            out.type = Value::Type::Bool;
            out.boolean = false;
            return true;
        }

        if (literal("null"))
        {
            out.type = Value::Type::Null;
            return true;
        }

        return fail("expected a value");
    }

    bool parseObject(Value& out)
    {
        out.type = Value::Type::Object;
        pos++; // '{'
        skip();

        if (pos < s.size() && s[pos] == '}')
        {
            pos++;
            return true;
        }

        while (true)
        {
            if (pos >= s.size() || s[pos] != '"')
                return fail("expected '\"' to start an object key");

            std::string key;

            if (!parseString(key))
                return false;

            skip();

            if (pos >= s.size() || s[pos] != ':')
                return fail("expected ':' after object key");

            pos++;
            skip();

            Value v;

            if (!parseValue(v))
                return false;

            out.members.emplace_back(std::move(key), std::move(v));

            skip();

            if (pos < s.size() && s[pos] == ',')
            {
                pos++;
                skip();
                continue;
            }

            if (pos < s.size() && s[pos] == '}')
            {
                pos++;
                return true;
            }

            return fail("expected ',' or '}' in object");
        }
    }

    bool parseArray(Value& out)
    {
        out.type = Value::Type::Array;
        pos++; // '['
        skip();

        if (pos < s.size() && s[pos] == ']')
        {
            pos++;
            return true;
        }

        while (true)
        {
            Value v;

            if (!parseValue(v))
                return false;

            out.items.push_back(std::move(v));

            skip();

            if (pos < s.size() && s[pos] == ',')
            {
                pos++;
                skip();
                continue;
            }

            if (pos < s.size() && s[pos] == ']')
            {
                pos++;
                return true;
            }

            return fail("expected ',' or ']' in array");
        }
    }

    // \uXXXX is not supported; config strings here are names, paths
    // and hex colors
    bool parseString(std::string& out)
    {
        pos++; // '"'

        while (pos < s.size())
        {
            char c = s[pos++];

            if (c == '"')
                return true;

            if (c == '\n')
                break;

            if (c != '\\')
            {
                out += c;
                continue;
            }

            if (pos >= s.size())
                break;

            switch (s[pos++])
            {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   return fail("unsupported string escape");
            }
        }

        return fail("unterminated string");
    }

    bool parseNumber(Value& out)
    {
        const char* start = s.c_str() + pos;
        char* end = nullptr;
        double v = strtod(start, &end);

        if (end == start)
            return fail("bad number");

        pos += end - start;
        out.type = Value::Type::Number;
        out.number = v;
        return true;
    }
};

inline bool parse(const std::string& text, Value& out, std::string& err)
{
    return Parser(text).parse(out, err);
}

} // namespace json
