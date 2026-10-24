#pragma once

// A small JSON reader for the arcade's answers: walks objects and arrays,
// reads strings and numbers, skips everything else. No tree is built, so a
// 130 KB mailbox costs nothing beyond its own bytes.

#include <stdlib.h>
#include <string.h>
#include <string>

namespace arcade
{

struct Json {
    const char *p, *end;
    bool ok = true;

    void ws()
    {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
            p++;
    }
    bool eat(char c)
    {
        ws();
        if (p < end && *p == c) {
            p++;
            return true;
        }
        return false;
    }
    static void utf8(std::string &out, uint32_t cp)
    {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | cp >> 6);
            out += (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | cp >> 12);
            out += (char)(0x80 | ((cp >> 6) & 63));
            out += (char)(0x80 | (cp & 63));
        } else {
            out += (char)(0xF0 | cp >> 18);
            out += (char)(0x80 | ((cp >> 12) & 63));
            out += (char)(0x80 | ((cp >> 6) & 63));
            out += (char)(0x80 | (cp & 63));
        }
    }
    uint32_t hex4(void)
    {
        if (end - p < 4) {
            ok = false;
            return 0;
        }
        char h[5] = {p[0], p[1], p[2], p[3], 0};
        p += 4;
        return (uint32_t)strtoul(h, nullptr, 16);
    }
    bool string(std::string *out)
    {
        ws();
        if (p >= end || *p != '"')
            return ok = false;
        p++;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                if (out)
                    *out += c;
                continue;
            }
            if (p >= end)
                return ok = false;
            char e = *p++;
            uint32_t cp = 0;
            switch (e) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u':
                cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                break;
            default: cp = (uint8_t)e;
            }
            if (out)
                utf8(*out, cp);
        }
        if (p >= end)
            return ok = false;
        p++;
        return true;
    }
    bool boolean(void)
    {
        ws();
        if (end - p >= 4 && !strncmp(p, "true", 4)) {
            p += 4;
            return true;
        }
        if (end - p >= 5 && !strncmp(p, "false", 5)) {
            p += 5;
            return false;
        }
        skip(); // null or anything else: false
        return false;
    }
    bool isString(void)
    {
        ws();
        return p < end && *p == '"';
    }
    bool isNumber(void)
    {
        ws();
        return p < end && (*p == '-' || (*p >= '0' && *p <= '9'));
    }
    std::string str(void) // a string, or "" for anything else
    {
        std::string v;
        if (isString())
            string(&v);
        else
            skip();
        return v;
    }
    double num(void) // a number, or 0 for anything else
    {
        if (isNumber())
            return number();
        skip();
        return 0;
    }
    double number(void)
    {
        ws();
        char *stop = nullptr;
        double v = strtod(p, &stop);
        if (stop == p)
            ok = false;
        p = stop;
        return v;
    }
    void skip(void)
    {
        ws();
        if (p >= end) {
            ok = false;
            return;
        }
        if (*p == '"') {
            string(nullptr);
        } else if (*p == '{' || *p == '[') {
            char close = *p == '{' ? '}' : ']';
            bool object = *p == '{';
            p++;
            if (eat(close))
                return;
            while (ok) {
                if (object) {
                    string(nullptr);
                    if (!eat(':'))
                        ok = false;
                }
                skip();
                if (eat(','))
                    continue;
                if (!eat(close))
                    ok = false;
                return;
            }
        } else if (!strncmp(p, "true", 4) && end - p >= 4) {
            p += 4;
        } else if (!strncmp(p, "false", 5) && end - p >= 5) {
            p += 5;
        } else if (!strncmp(p, "null", 4) && end - p >= 4) {
            p += 4;
        } else {
            number();
        }
    }
    // calls each(key) for every member; each() must consume the value
    template <class F> void object(F each)
    {
        if (!eat('{')) {
            ok = false;
            return;
        }
        if (eat('}'))
            return;
        while (ok) {
            std::string key;
            if (!string(&key) || !eat(':'))
                return void(ok = false);
            each(key);
            if (eat(','))
                continue;
            if (!eat('}'))
                ok = false;
            return;
        }
    }
    template <class F> void array(F each)
    {
        if (!eat('[')) {
            ok = false;
            return;
        }
        if (eat(']'))
            return;
        while (ok) {
            each();
            if (eat(','))
                continue;
            if (!eat(']'))
                ok = false;
            return;
        }
    }
};

} // namespace arcade
