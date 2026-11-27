// Hoptics-MC: the small text work the web server and the backups need, kept free of the Arduino core so the PC tests
// run the same code (harness/parse): a path on the card as the page sends it, the URL's %-escapes, and JSON read one
// object at a time (the page's requests and the backup files are flat objects and arrays of them). MIT licence.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace hxparse
{
// %xx undone ('+' stays '+': the page encodes with encodeURIComponent). False: it doesn't fit, a % is broken, or the
// result would hold a NUL
bool urlDecode(const char *in, char *out, size_t n);
// a path on the card from the page, made plain: "/" or "/name/name". Refused (false): not starting with '/', a name
// that is "." or "..", or ends in a dot or a space (FAT drops those, so it would be another file), control characters,
// a backslash or one of :*?"<>| (not allowed on FAT), a name over 255 bytes, or the whole not fitting in n.
// Repeated and trailing slashes are dropped
bool cleanPath(const char *in, char *out, size_t n);
const char *baseName(const char *path); // the last name in a clean path ("" for "/")

// ---- JSON
struct Value {
    const char *p = nullptr; // where it starts (a string: just after its opening quote)
    size_t n = 0;            // how long (a string: up to its closing quote, escapes still in)
    char kind = 0;           // 's' string, 'n' number, 'o' object, 'a' array, 'b' true or false, 'z' null
};
const char *ws(const char *p, const char *end);
// one value at p (no spaces before it): what it is, and the character after it; nullptr: not JSON (or nested deeper
// than 16)
const char *value(const char *p, const char *end, Value &v);
bool is(const Value &key, const char *name); // a key (as read, escapes not undone) is this name
bool text(const Value &v, char *out, size_t n); // a string, decoded to UTF-8; false: not a string, or too long
double number(const Value &v, double def);      // a number (def if it isn't one)
bool hex(const Value &v, uint8_t *out, size_t n); // a string of exactly 2n hex digits
// the JSON string whose opening quote is just before p, decoded to UTF-8 (\n, \t and control characters become
// spaces, \uXXXX and surrogate pairs become UTF-8). False: too long for out, or it never ends
bool jsonText(const char *p, char *out, size_t n);
// s as a JSON string. Text from the radio is strangers' text: quotes, backslashes, control characters and <>& are
// escaped, so it can only ever be a string in the page
void jsonString(std::string &o, const char *s);

// the object at p (spaces before it allowed): f(key, value) for each field, in order; p ends just after it
template <class F> bool object(const char *&p, const char *end, F &&f)
{
    p = ws(p, end);
    if (p >= end || *p != '{')
        return false;
    p = ws(p + 1, end);
    if (p < end && *p == '}')
        return ++p, true;
    for (;;) {
        Value k, v;
        p = value(p, end, k);
        if (!p || k.kind != 's')
            return false;
        p = ws(p, end);
        if (p >= end || *p != ':')
            return false;
        p = value(ws(p + 1, end), end, v);
        if (!p)
            return false;
        f(k, v);
        p = ws(p, end);
        if (p < end && *p == ',') {
            p = ws(p + 1, end);
            continue;
        }
        if (p < end && *p == '}')
            return ++p, true;
        return false;
    }
}

// the array at p: f(value) for each element, in order; p ends just after it
template <class F> bool array(const char *&p, const char *end, F &&f)
{
    p = ws(p, end);
    if (p >= end || *p != '[')
        return false;
    p = ws(p + 1, end);
    if (p < end && *p == ']')
        return ++p, true;
    for (;;) {
        Value v;
        p = value(p, end, v);
        if (!p)
            return false;
        f(v);
        p = ws(p, end);
        if (p < end && *p == ',') {
            p = ws(p + 1, end);
            continue;
        }
        if (p < end && *p == ']')
            return ++p, true;
        return false;
    }
}
} // namespace hxparse
