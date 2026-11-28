// libFuzzer: the live feed's message parser and server-address splitter (hoptics/net/hx_feed), on random input under
// ASan/UBSan. A feed message comes from a server on the internet. Run: bash harness/fuzz/run.sh [seconds]
#include "hx_feed.h"
#include <stdint.h>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hxfeed::Item it;
    hxfeed::parse((const char *)data, size, it); // not NUL-terminated: the parser must keep to the length
    std::string s((const char *)data, size);
    bool tls;
    char host[64], path[64];
    uint16_t port;
    hxfeed::splitUrl(s.c_str(), tls, host, sizeof(host), port, path, sizeof(path));
    return 0;
}
