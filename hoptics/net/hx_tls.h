// Hoptics-MC web page over HTTPS: the node makes its own key and certificate on first use (kept in NVS) and serves the
// page encrypted, so the PIN, the login cookie and backups can't be read off the Wi-Fi. The certificate's SHA-256
// fingerprint is shown so you can check the browser is talking to your own node. MIT licence.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hxtls
{
// loads the key and certificate, or makes them (a second or so the first time). host: the node's mDNS name without
// ".local", put in the certificate. False if TLS can't be set up (the page then stays off rather than unencrypted)
bool begin(const char *host);
bool ready();
const char *fingerprint();           // "AB:CD:..." (SHA-256 of the certificate), empty until begin()
const uint8_t *certDer(size_t &len); // to download the certificate and trust it on a phone or computer
void forget();                       // a new key and certificate on the next begin(): browsers that trusted the old one must again

struct Stats {
    uint32_t handshakes = 0, failed = 0;
    uint32_t lastMs = 0, maxMs = 0; // handshake time
    uint8_t open = 0;               // sessions open now
    int lastErr = 0;                // mbedTLS's code for the last failed handshake
};
Stats stats();
// esp_http_server open_fn (hx_web): a TLS handshake on each new socket, then the socket's reads and writes go through it
int onOpen(void *server, int fd);
} // namespace hxtls
