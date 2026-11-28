#include "hx_feed.h"
#include <cstdio>
#include <cstring>
#include <cassert>
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
    bool tls; char h[64], p[64]; uint16_t port;
    const char *urls[] = {"corescope.sacmesh.org", "wss://corescope.sacmesh.org/ws", "ws://192.168.1.5:3000", "https://x.org:8443/live/ws", " ws://h/", "", "wss://:99/ws", "h:0", "WS://h/ws"};
    for (auto u : urls) { bool ok = hxfeed::splitUrl(u, tls, h, sizeof(h), port, p, sizeof(p)); printf("%-34s -> %d %s %s %u %s\n", u, ok, ok&&tls?"tls":"   ", ok?h:"", ok?port:0, ok?p:""); }
    puts("all good");
}
