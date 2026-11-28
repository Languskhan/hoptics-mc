// libFuzzer: the web server's text helpers (hoptics/net/hx_parse) on random input, under ASan/UBSan. Everything here
// reads what a browser or a backup file sends. Run: bash harness/fuzz/run.sh [seconds]
#include "hx_parse.h"
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    std::string s((const char *)data, size); // NUL-terminated copy, as the server makes of a body
    const char *p = s.c_str(), *end = p + size;
    char out[256];
    hxparse::urlDecode(p, out, sizeof(out));
    if (hxparse::cleanPath(p, out, sizeof(out)))
        hxparse::baseName(out);
    hxparse::localHost(p);
    hxparse::jsonText(p, out, sizeof(out));
    hxparse::jsonText(p, out, 1);
    std::string o;
    hxparse::jsonString(o, p);
    // a backup: objects inside arrays inside an object, each field read the way webRestore reads it
    const char *q = p;
    hxparse::object(q, end, [&](const hxparse::Value &k, const hxparse::Value &v) {
        (void)k;
        uint8_t key[32];
        hxparse::text(v, out, sizeof(out));
        hxparse::number(v, 0);
        hxparse::integer(v, -1000, 1000, 0);
        hxparse::hex(v, key, 16);
        if (v.kind == 'a') {
            const char *r = v.p;
            hxparse::array(r, v.p + v.n, [&](const hxparse::Value &e) {
                const char *t = e.p;
                if (e.kind == 'o')
                    hxparse::object(t, e.p + e.n, [&](const hxparse::Value &, const hxparse::Value &x) { hxparse::text(x, out, 32); });
            });
        }
    });
    q = p;
    hxparse::array(q, end, [](const hxparse::Value &) {});
    return 0;
}
