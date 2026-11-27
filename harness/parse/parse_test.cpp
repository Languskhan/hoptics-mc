// The web server's text helpers (hoptics/net/hx_parse): card paths from the page, %-escapes, and JSON read a field at a
// time, including what a hostile page could send. Run: bash harness/parse/run.sh (ASan/UBSan, exits non-zero on failure)
#include "hx_parse.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int n = 0;
#define OK(c)                                                                                                              \
    do {                                                                                                                   \
        if (!(c)) {                                                                                                        \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                                           \
            return 1;                                                                                                      \
        }                                                                                                                  \
        n++;                                                                                                               \
    } while (0)

static std::string clean(const char *in, size_t cap = 256)
{
    std::vector<char> b(cap);
    return hxparse::cleanPath(in, b.data(), cap) ? std::string(b.data()) : std::string("!");
}

int main()
{
    // paths: the root, plain names, slashes tidied
    OK(clean("/") == "/");
    OK(clean("//") == "/");
    OK(clean("/maps/osm/12") == "/maps/osm/12");
    OK(clean("/maps//osm/") == "/maps/osm");
    OK(clean("/Caf\xc3\xa9 notes.txt") == "/Caf\xc3\xa9 notes.txt"); // UTF-8 names are fine
    OK(clean("/.hidden") == "/.hidden");
    // ...and everything that could leave the card or name another file
    for (const char *bad : {"", "maps", "../x", "/..", "/../x", "/a/../../b", "/a/./b", "/.", "/a/..", "/a.", "/a ", "/a\\b",
                            "/c:/x", "/a*", "/a?", "/a\"b", "/a<b", "/a>b", "/a|b", "/a\nb", "/a\tb", "/a\x7f", "/a\x01"})
        OK(clean(bad) == "!");
    OK(clean("/abcdef", 7) == "!"); // doesn't fit (with its NUL)
    OK(clean("/abcde", 7) == "/abcde");
    std::string longName = "/" + std::string(256, 'a');
    OK(clean(longName.c_str(), 400) == "!");
    longName.pop_back();
    OK(clean(longName.c_str(), 400) == longName);
    OK(!strcmp(hxparse::baseName("/a/b.txt"), "b.txt") && !strcmp(hxparse::baseName("/"), ""));

    // %-escapes
    char d[32];
    OK(hxparse::urlDecode("%2Fmaps%2Fa%20b", d, sizeof(d)) && !strcmp(d, "/maps/a b"));
    OK(hxparse::urlDecode("a+b", d, sizeof(d)) && !strcmp(d, "a+b"));
    OK(!hxparse::urlDecode("%2", d, sizeof(d)) && !hxparse::urlDecode("%zz", d, sizeof(d)) && !hxparse::urlDecode("a%00b", d, sizeof(d)));
    OK(!hxparse::urlDecode("abcd", d, 4) && hxparse::urlDecode("abc", d, 4));
    // an escaped ".." is still refused once decoded
    OK(hxparse::urlDecode("%2F..%2Fx", d, sizeof(d)) && clean(d) == "!");

    // JSON: an object's fields in order, nested values skipped whole, strings with escapes inside
    const char *js = " {\"a\":1, \"s\":\"x\\\"}y\", \"o\":{\"k\":[1,{\"z\":\"]\"}]}, \"t\":true, \"n\":null, \"f\":-2.5e1, \"e\":[]} tail";
    const char *p = js;
    std::vector<std::string> keys;
    double f = 0;
    char s[16] = "";
    OK(hxparse::object(p, js + strlen(js), [&](const hxparse::Value &k, const hxparse::Value &v) {
        keys.emplace_back(k.p, k.n);
        if (hxparse::is(k, "f"))
            f = hxparse::number(v, 0);
        if (hxparse::is(k, "s"))
            hxparse::text(v, s, sizeof(s));
        if (hxparse::is(k, "o"))
            assert(v.kind == 'o' && v.n == strlen("{\"k\":[1,{\"z\":\"]\"}]}"));
    }));
    OK(keys.size() == 7 && keys[2] == "o" && keys[6] == "e");
    OK(f == -25 && !strcmp(s, "x\"}y"));
    OK(!strcmp(p, " tail"));
    // arrays of objects (the backup's contacts)
    const char *arr = "[{\"i\":0},{\"i\":1} , {\"i\":2}]";
    p = arr;
    int sum = 0, count = 0;
    OK(hxparse::array(p, arr + strlen(arr), [&](const hxparse::Value &v) {
        const char *q = v.p;
        hxparse::object(q, v.p + v.n, [&](const hxparse::Value &k, const hxparse::Value &x) {
            if (hxparse::is(k, "i"))
                sum += (int)hxparse::number(x, 0);
        });
        count++;
    }));
    OK(count == 3 && sum == 3);
    // broken or hostile JSON is refused, never read past its end
    for (const char *bad : {"{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{\"a\":\"x}", "{a:1}", "{\"a\":1 \"b\":2}", "[1,2", "{\"a\":[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]}"}) {
        const char *q = bad;
        OK(!hxparse::object(q, bad + strlen(bad), [](const hxparse::Value &, const hxparse::Value &) {}));
    }
    {
        const char cut[] = {'{', '"', 'a', '"', ':', '"', '\\'}; // ends inside an escape: no read past the end
        const char *q = cut;
        OK(!hxparse::object(q, cut + sizeof(cut), [](const hxparse::Value &, const hxparse::Value &) {}));
    }
    // numbers and hex
    hxparse::Value v;
    OK(hxparse::value("12abc", "12abc" + 5, v) && hxparse::number(v, -1) == 12);
    OK(hxparse::value("1e999x", "1e999x" + 6, v) && std::isinf(hxparse::number(v, 0)));
    uint8_t h[2];
    OK(hxparse::value("\"0aFf\"", "\"0aFf\"" + 6, v) && hxparse::hex(v, h, 2) && h[0] == 0x0a && h[1] == 0xff);
    OK(hxparse::value("\"0aFg\"", "\"0aFg\"" + 6, v) && !hxparse::hex(v, h, 2));
    OK(!hxparse::hex(v, h, 1));
    // text: escapes, emoji, too long
    char t[8];
    OK(hxparse::jsonText("a\\nb\\u00e9\"", t, sizeof(t)) && !strcmp(t, "a b\xc3\xa9"));
    OK(hxparse::jsonText("\\ud83d\\ude00\"", t, sizeof(t)) && !strcmp(t, "\xf0\x9f\x98\x80"));
    OK(!hxparse::jsonText("abcdefgh\"", t, sizeof(t)) && !hxparse::jsonText("abc", t, sizeof(t)));
    // writing: what the page shows can only ever be a string
    std::string o;
    hxparse::jsonString(o, "a\"b\\c<d>&\n");
    OK(o == "\"a\\\"b\\\\c\\u003cd\\u003e\\u0026\\u000a\"");
    // a round trip through both
    p = o.c_str();
    OK(hxparse::value(p, p + o.size(), v) && hxparse::text(v, s, sizeof(s)) && !strcmp(s, "a\"b\\c<d>& "));

    printf("hx_parse: %d checks passed\n", n);
    return 0;
}
