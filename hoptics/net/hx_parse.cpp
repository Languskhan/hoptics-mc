// Hoptics-MC text helpers (see hx_parse.h). MIT licence.
#include "hx_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace hxparse
{
namespace
{
int hexDigit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
} // namespace

bool urlDecode(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '%') {
            const int a = hexDigit(p[1]), b = a < 0 ? -1 : hexDigit(p[2]);
            if (b < 0)
                return false;
            c = (char)(a * 16 + b);
            if (!c)
                return false;
            p += 2;
        }
        if (k + 1 >= n)
            return false;
        out[k++] = c;
    }
    if (!n)
        return false;
    out[k] = 0;
    return true;
}

bool cleanPath(const char *in, char *out, size_t n)
{
    if (!in || in[0] != '/' || n < 2)
        return false;
    size_t k = 0;
    const char *p = in;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/') {
            const unsigned char c = (unsigned char)*p++;
            if (c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c))
                return false;
        }
        const size_t len = (size_t)(p - s);
        if (len > 255 || s[len - 1] == '.' || s[len - 1] == ' ') // ".", ".." and any name FAT would change
            return false;
        if (k + 1 + len + 1 > n)
            return false;
        out[k++] = '/';
        memcpy(out + k, s, len);
        k += len;
    }
    if (!k)
        out[k++] = '/';
    out[k] = 0;
    return true;
}

const char *baseName(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// ---- JSON
const char *ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

const char *value(const char *p, const char *end, Value &v)
{
    v = Value();
    if (!p || p >= end)
        return nullptr;
    const char c = *p;
    if (c == '"') {
        const char *s = ++p;
        while (p < end && *p != '"') {
            if (*p == '\\' && ++p >= end)
                return nullptr;
            p++;
        }
        if (p >= end)
            return nullptr;
        v.p = s, v.n = (size_t)(p - s), v.kind = 's';
        return p + 1;
    }
    if (c == '{' || c == '[') { // nested: to its matching end, skipping what is inside strings
        int depth = 0;
        const char *s = p;
        for (; p < end; p++) {
            if (*p == '"') {
                for (p++; p < end && *p != '"'; p++)
                    if (*p == '\\')
                        p++;
                if (p >= end)
                    return nullptr;
            } else if (*p == '{' || *p == '[') {
                if (++depth > 16)
                    return nullptr;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) {
                    v.p = s, v.n = (size_t)(p + 1 - s), v.kind = c == '{' ? 'o' : 'a';
                    return p + 1;
                }
            }
        }
        return nullptr;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *s = p;
        while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9')))
            p++;
        v.p = s, v.n = (size_t)(p - s), v.kind = 'n';
        return p;
    }
    static const struct {
        const char *w;
        char kind;
    } words[] = {{"true", 'b'}, {"false", 'b'}, {"null", 'z'}};
    for (const auto &w : words) {
        const size_t len = strlen(w.w);
        if ((size_t)(end - p) >= len && !memcmp(p, w.w, len)) {
            v.p = p, v.n = len, v.kind = w.kind;
            return p + len;
        }
    }
    return nullptr;
}

bool is(const Value &key, const char *name)
{
    return key.kind == 's' && strlen(name) == key.n && !memcmp(key.p, name, key.n);
}

bool text(const Value &v, char *out, size_t n)
{
    return v.kind == 's' && jsonText(v.p, out, n);
}

double number(const Value &v, double def)
{
    if (v.kind == 'b')
        return *v.p == 't' ? 1 : 0;
    if (v.kind != 'n' || v.n >= 32)
        return def;
    char b[32];
    memcpy(b, v.p, v.n);
    b[v.n] = 0;
    char *e;
    const double d = strtod(b, &e);
    return *e ? def : d;
}

bool hex(const Value &v, uint8_t *out, size_t n)
{
    if (v.kind != 's' || v.n != n * 2)
        return false;
    for (size_t i = 0; i < n; i++) {
        const int a = hexDigit(v.p[i * 2]), b = hexDigit(v.p[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}

bool jsonText(const char *p, char *out, size_t n)
{
    size_t k = 0;
    while (*p && *p != '"') {
        uint32_t c = (uint8_t)*p++;
        if (c == '\\') {
            const char e = *p++;
            if (e == 'n' || e == 'r' || e == 't')
                c = ' ';
            else if (e == 'u') {
                char h[5] = {};
                for (int i = 0; i < 4 && *p; i++)
                    h[i] = *p++;
                c = strtoul(h, nullptr, 16);
                if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u') { // a surrogate pair (emoji)
                    char l[5] = {};
                    for (int i = 0; i < 4 && p[2 + i]; i++)
                        l[i] = p[2 + i];
                    const uint32_t lo = strtoul(l, nullptr, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (c < 0x20)
                    c = ' ';
                char u[4];
                int m = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
                if (m == 1) u[0] = (char)c;
                else if (m == 2) u[0] = (char)(0xC0 | c >> 6), u[1] = (char)(0x80 | (c & 63));
                else if (m == 3) u[0] = (char)(0xE0 | c >> 12), u[1] = (char)(0x80 | ((c >> 6) & 63)), u[2] = (char)(0x80 | (c & 63));
                else u[0] = (char)(0xF0 | c >> 18), u[1] = (char)(0x80 | ((c >> 12) & 63)), u[2] = (char)(0x80 | ((c >> 6) & 63)), u[3] = (char)(0x80 | (c & 63));
                if (k + m >= n)
                    return false;
                memcpy(out + k, u, m);
                k += m;
                continue;
            } else if (e)
                c = (uint8_t)e;
            else
                return false;
        }
        if (c < 0x20)
            c = ' ';
        if (k + 1 >= n)
            return false;
        out[k++] = (char)c;
    }
    out[k] = 0;
    return *p == '"';
}

void jsonString(std::string &o, const char *s)
{
    o += '"';
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20 || c == '<' || c == '>' || c == '&') {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else
            o += (char)c;
    }
    o += '"';
}
} // namespace hxparse
