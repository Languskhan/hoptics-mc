// Live feed: packets the region's observers hear, from a CoreScope server's public WebSocket (the one its live map
// uses). Each message carries the raw packet as heard, which observer heard it and how well. The radio decodes the
// raw packets itself with its own channel keys, so nothing secret is needed from the server, and nothing goes back.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hxfeed
{

struct Item {
    uint8_t raw[255];    // the packet as the observer heard it (header, path, payload)
    uint8_t len = 0;
    uint8_t obsKey[6] = {}; // the observer's public key prefix
    char obsName[24] = {};
    int8_t snr4 = 0;     // at the observer, SNR * 4
};

enum class State : uint8_t { OFF, NO_WIFI, CONNECTING, LIVE, FAILED };

// Pull the fields out of one feed message (JSON text). False when it is not a packet or does not fit.
bool parse(const char *json, size_t n, Item &out);

// "scope.sacmesh.org", "wss://host/ws" or "ws://192.168.1.5:3000/ws" -> its parts. False when it can't be used.
bool splitUrl(const char *url, bool &tls, char *host, size_t hostN, uint16_t &port, char *path, size_t pathN);

// The connection runs in its own task (ESP32); these are safe to call from the main loop.
void start(const char *url, const char *userAgent); // (re)connects to this server whenever Wi-Fi is up
void stop();
bool pop(Item &out);       // next packet heard, oldest first
State state();
uint32_t packets();        // received since start()
const char *lastError();   // why the last attempt failed ("" when fine)
uint32_t busyUs();          // microseconds the feed's task has spent working (not waiting), for the status line

} // namespace hxfeed
