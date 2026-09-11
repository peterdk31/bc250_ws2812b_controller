#pragma once

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"

// Writing a dashboard edit back into the config file (README "Editing curves
// from the phone"). The phone's edits — a fan curve, the strip's brightness,
// a scene's color — land in the daemon as values; this puts them in the file,
// so the config stays the one place the box is described and a restart reads
// the edit like any other setting.
//
// Only the bytes of the edited value are replaced: the caller names a path
// into the file's JSON (`{"fans"}`, `{"rules", "3", "settings"}`), the value's
// span in the text is found, and the new value is printed over it in the
// file's own four-space style. Everything around it — the user's ordering,
// spacing, the rest of the file — stays byte for byte as it was.
//
// One Writer per config file, shared by every module that edits it: it knows
// whether the file is writable (the dashboards turn read-only when it isn't,
// or when a write fails — the phone stops offering saves that can't stick),
// keeps the first rewrite of a daemon's life as <config>.bak beside it, and
// tells the main loop's file watch that the mtime it is about to see was our
// own doing (takeWrote) rather than a hand edit to reload.
namespace cfgedit
{
// a mutable member of an object, appended when absent
inline json::Value& member(json::Value& obj, const std::string& key)
{
    for (auto& m : obj.members)
        if (m.first == key)
            return m.second;
    obj.members.emplace_back(key, json::Value());
    return obj.members.back().second;
}

inline json::Value number(double v)
{
    json::Value n;
    n.type = json::Value::Type::Number;
    n.number = v;
    return n;
}

inline json::Value string(const std::string& v)
{
    json::Value n;
    n.type = json::Value::Type::String;
    n.text = v;
    return n;
}

inline json::Value boolean(bool v)
{
    json::Value n;
    n.type = json::Value::Type::Bool;
    n.boolean = v;
    return n;
}

// a JSON string literal's body
inline std::string escape(const std::string& s)
{
    std::string out;
    char buf[8];
    for (unsigned char ch : s)
    {
        if (ch == '"' || ch == '\\')
            out += '\\', out += (char)ch;
        else if (ch == '\n')
            out += "\\n";
        else if (ch < 0x20)
            snprintf(buf, sizeof buf, "\\u%04x", ch), out += buf;
        else
            out += (char)ch;
    }
    return out;
}

// pretty-print a value the way the config is written: four-space indents,
// one member or item per line. `depth` is the nesting of the value's own
// opening bracket (a top-level block sits at 1).
inline void print(const json::Value& v, int depth, std::string& out)
{
    using T = json::Value::Type;
    const std::string pad(4 * depth, ' '), padIn(4 * (depth + 1), ' ');
    char buf[32];

    switch (v.type)
    {
        case T::Null: out += "null"; break;
        case T::Bool: out += v.boolean ? "true" : "false"; break;
        case T::Number:
            if (v.number == (double)(long long)v.number && fabs(v.number) < 1e15)
                snprintf(buf, sizeof buf, "%lld", (long long)v.number);
            else
                snprintf(buf, sizeof buf, "%.10g", v.number);
            out += buf;
            break;
        case T::String: out += "\"" + escape(v.text) + "\""; break;
        case T::Array:
            if (v.items.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.items.size(); i++)
            {
                out += padIn;
                print(v.items[i], depth + 1, out);
                out += i + 1 < v.items.size() ? ",\n" : "\n";
            }
            out += pad + "]";
            break;
        case T::Object:
            if (v.members.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.members.size(); i++)
            {
                out += padIn + "\"" + escape(v.members[i].first) + "\": ";
                print(v.members[i].second, depth + 1, out);
                out += i + 1 < v.members.size() ? ",\n" : "\n";
            }
            out += pad + "}";
            break;
    }
}

// the end of the JSON value starting at t[i] (a bracketed value, a string or
// a bare scalar), respecting strings and escapes
inline size_t skipValue(const std::string& t, size_t i)
{
    auto skipString = [&](size_t j) {
        for (j++; j < t.size(); j++)
        {
            if (t[j] == '\\') j++;
            else if (t[j] == '"') return j + 1;
        }
        return j;
    };
    if (i >= t.size())
        return i;
    if (t[i] == '"')
        return skipString(i);
    if (t[i] == '{' || t[i] == '[')
    {
        int depth = 0;
        for (; i < t.size(); i++)
        {
            if (t[i] == '"') { i = skipString(i) - 1; continue; }
            if (t[i] == '{' || t[i] == '[') depth++;
            else if ((t[i] == '}' || t[i] == ']') && --depth == 0) return i + 1;
        }
        return i;
    }
    while (i < t.size() && !strchr(",}] \t\r\n", t[i]))
        i++;
    return i;
}

inline size_t skipSpace(const std::string& t, size_t i)
{
    while (i < t.size() && isspace((unsigned char)t[i]))
        i++;
    return i;
}

// the byte span [start, end) of the value at `path` inside the value whose
// text starts at t[from] — an object member by key, an array item by index
// (a decimal segment), one segment at a time
inline bool spanOf(const std::string& t, size_t from, const std::vector<std::string>& path,
                   size_t at, size_t& start, size_t& end)
{
    if (at == path.size())
    {
        start = from;
        end = skipValue(t, from);
        return end > start;
    }

    const std::string& seg = path[at];
    size_t i = skipSpace(t, from);
    if (i >= t.size())
        return false;

    if (t[i] == '{')
    {
        for (i = skipSpace(t, i + 1); i < t.size() && t[i] != '}';)
        {
            if (t[i] != '"')
                return false;
            size_t e = skipValue(t, i);
            std::string key = t.substr(i + 1, e - i - 2);
            i = skipSpace(t, e);
            if (i >= t.size() || t[i] != ':')
                return false;
            i = skipSpace(t, i + 1);
            if (key == seg)
                return spanOf(t, i, path, at + 1, start, end);
            i = skipSpace(t, skipValue(t, i));
            if (i < t.size() && t[i] == ',')
                i = skipSpace(t, i + 1);
        }
        return false;
    }

    if (t[i] == '[')
    {
        char* endp = nullptr;
        long want = strtol(seg.c_str(), &endp, 10);
        if (!endp || *endp || want < 0)
            return false;
        long idx = 0;
        for (i = skipSpace(t, i + 1); i < t.size() && t[i] != ']'; idx++)
        {
            if (idx == want)
                return spanOf(t, i, path, at + 1, start, end);
            i = skipSpace(t, skipValue(t, i));
            if (i < t.size() && t[i] == ',')
                i = skipSpace(t, i + 1);
        }
        return false;
    }

    return false;
}

class Writer
{
public:
    // path "" = no file (a test, --fan-status): edits are refused
    void open(const std::string& path)
    {
        path_ = path;
        writable_ = !path.empty() && access(path.c_str(), W_OK) == 0;
    }

    const std::string& path() const { return path_; }
    bool writable() const { return writable_; }

    // did a write succeed since the last call? The main loop's file watch
    // uses this to tell our own write from an edit it should reload.
    bool takeWrote()
    {
        bool w = wrote_;
        wrote_ = false;
        return w;
    }

    // print `v` over the value at `path` in the file. `who` prefixes the
    // journal lines ("fans", "strip"). A failure is said, turns the writer
    // read-only, and leaves the file as it was; the caller's edit then runs
    // until the next restart.
    bool write(const std::vector<std::string>& path, const json::Value& v, const char* who)
    {
        std::string why;
        std::ifstream in(path_);
        std::stringstream buf;
        if (in.is_open())
            buf << in.rdbuf();
        std::string text = buf.str();
        size_t a = 0, b = 0;
        if (text.empty())
            why = "can't read it";
        else if (!spanOf(text, 0, path, 0, a, b))
            why = "\"" + joined(path) + "\" not found in the file — was it changed under the daemon?";

        // the first rewrite of this daemon's life keeps the file as it found
        // it, beside it, as insurance against this very code — a tuned config
        // is the one thing on the box that is hard to recreate
        if (why.empty() && !backedUp_)
        {
            std::string bak = path_ + ".bak";
            FILE* bf = fopen(bak.c_str(), "w");
            if (bf)
            {
                fwrite(text.data(), 1, text.size(), bf);
                fclose(bf);
            }
            backedUp_ = true;
        }

        bool ok = why.empty();
        if (ok)
        {
            std::string printed;
            print(v, (int)path.size(), printed);
            text = text.substr(0, a) + printed + text.substr(b);
        }

        std::string tmp = path_ + ".tmp";
        struct stat st;
        FILE* f = ok ? fopen(tmp.c_str(), "w") : nullptr;
        if (ok && !f)
            why = strerror(errno), ok = false;
        if (ok)
        {
            ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0 &&
                 fsync(fileno(f)) == 0;
            ok = fclose(f) == 0 && ok;
            if (ok && stat(path_.c_str(), &st) == 0)
                chmod(tmp.c_str(), st.st_mode & 07777);
            if (ok && rename(tmp.c_str(), path_.c_str()) != 0)
                ok = false;
            if (!ok)
            {
                why = strerror(errno);
                unlink(tmp.c_str());
            }
        }

        if (!ok)
        {
            fprintf(stderr, "%s: can't write the edit to %s: %s — it runs until "
                            "restart; dashboard now read-only\n",
                    who, path_.c_str(), why.c_str());
            writable_ = false;
            return false;
        }
        wrote_ = true;
        fprintf(stderr, "%s: %s updated\n", who, path_.c_str());
        return true;
    }

private:
    static std::string joined(const std::vector<std::string>& path)
    {
        std::string s;
        for (auto& p : path)
            s += (s.empty() ? "" : ".") + p;
        return s;
    }

    std::string path_;
    bool writable_ = false;
    bool wrote_ = false;
    bool backedUp_ = false;
};
} // namespace cfgedit
