#include "hx_feed.h"
#include <cstdio>
#include <cstring>
#include <cassert>
#include <sys/mman.h>
#include <unistd.h>
int main() {
    const char *j = "{\"type\":\"packet\",\"data\":{\"id\":42,\"raw_hex\":\"1501AB11C3D4E5F6\",\"hash\":\"abc\",\"observer_id\":\"7f3a9c11223344556677\","
                    "\"observer_name\":\"S2 \\\"Elk\\\" Obs\\u00e9rver long name here\",\"snr\":-7.25,\"rssi\":-110,\"decoded\":{\"payload\":{}},"
                    "\"packet\":{\"raw_hex\":\"FFFF\"}}}";
    hxfeed::Item it;
    assert(hxfeed::parse(j, strlen(j), it));
    printf("len=%d raw0=%02x raw7=%02x obs=%02x%02x..%02x name='%s' snr4=%d\n", it.len, it.raw[0], it.raw[7], it.obsKey[0], it.obsKey[1], it.obsKey[5], it.obsName, it.snr4);
    assert(it.len == 8 && it.raw[0] == 0x15 && it.raw[7] == 0xF6 && it.obsKey[0] == 0x7f && it.obsKey[5] == 0x33 && it.snr4 == -29);
    const char *n = "{\"type\":\"packet\",\"data\":{\"raw_hex\":\"1501\",\"snr\":null}}";
    assert(hxfeed::parse(n, strlen(n), it) && it.snr4 == 0 && it.obsName[0] == 0);
    const char *bad = "{\"type\":\"stats\",\"data\":{}}";
    assert(!hxfeed::parse(bad, strlen(bad), it));
    const char *odd = "{\"raw_hex\":\"15G1\"}";
    assert(!hxfeed::parse(odd, strlen(odd), it));
    const char *cut = "{\"raw_hex\":\"1501AB";  // truncated message
    assert(hxfeed::parse(cut, strlen(cut), it) && it.len == 3);
    const char *loud = "{\"raw_hex\":\"1501\",\"snr\":725}";  // past what snr4 holds: clamped, not wrapped
    assert(hxfeed::parse(loud, strlen(loud), it) && it.snr4 == 127);
    const char *quiet = "{\"raw_hex\":\"1501\",\"snr\":-725}";
    assert(hxfeed::parse(quiet, strlen(quiet), it) && it.snr4 == -128);
    const char *huge = "{\"raw_hex\":\"1501\",\"snr\":1e999}";
    assert(hxfeed::parse(huge, strlen(huge), it) && it.snr4 == 127);
    const char *spaced = "{\"raw_hex\":\"1501\",\"snr\":\n\t-7.25}";  // whitespace before the number still reads
    assert(hxfeed::parse(spaced, strlen(spaced), it) && it.snr4 == -29);
    const char *notHex = "{\"raw_hex\":\"1501\",\"observer_id\":\"7f3a9c1122zz44556677\"}";  // a bad ID gives no key
    assert(hxfeed::parse(notHex, strlen(notHex), it));
    for (int i = 0; i < 6; i++) assert(it.obsKey[i] == 0);
    memset(it.obsName, 'x', sizeof(it.obsName));  // stale bytes from an earlier message
    const char *open = "{\"raw_hex\":\"1501\",\"observer_name\":\"a name with no closing quote at all";
    assert(hxfeed::parse(open, strlen(open), it) && it.obsName[0] == 0);
    const char *stale = "{\"raw_hex\":\"1501\",\"snr\":199";  // the message ends after the 1; "99" is old buffer
    assert(hxfeed::parse(stale, strlen(stale) - 2, it) && it.snr4 == 4);
    {   // a message that fills the 12 KB buffer and ends in digits, right against an unreadable page
        const size_t full = 12288, pg = (size_t)sysconf(_SC_PAGESIZE), span = (full + pg - 1) / pg * pg;
        char *m = (char *)mmap(nullptr, span + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(m != MAP_FAILED && mprotect(m + span, pg, PROT_NONE) == 0);
        char *msg = m + span - full;
        const char *head = "{\"raw_hex\":\"1501\",\"pad\":\"", *tail = "\",\"snr\":12";
        memset(msg, 'p', full);
        memcpy(msg, head, strlen(head));
        memcpy(msg + full - strlen(tail), tail, strlen(tail));
        assert(hxfeed::parse(msg, full, it) && it.snr4 == 48);
        munmap(m, span + pg);
    }
    bool tls; char h[64], p[64]; uint16_t port;
    const char *urls[] = {"corescope.sacmesh.org", "wss://corescope.sacmesh.org/ws", "ws://192.168.1.5:3000", "https://x.org:8443/live/ws", " ws://h/", "", "wss://:99/ws", "h:0", "WS://h/ws"};
    for (auto u : urls) { bool ok = hxfeed::splitUrl(u, tls, h, sizeof(h), port, p, sizeof(p)); printf("%-34s -> %d %s %s %u %s\n", u, ok, ok&&tls?"tls":"   ", ok?h:"", ok?port:0, ok?p:""); }
    puts("all good");
}
