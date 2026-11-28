// Hoptics-MC web page: the node serves its dashboard (hoptics/web, packed by tools/make_web.py) on the local Wi-Fi.
// Signing in takes the 6-digit PIN shown on the node's screen; a session lasts 12 hours in that browser.
//
// The page's data comes from the main loop, which owns it: a request waits (up to a second) while the main loop
// builds the answer on its next pass (take() / answer()). Everything else (files, sign-in) is answered by the server's
// own task. The microSD card's files (Files) are read and written through the main loop too, a piece at a time
// (hx_sd.h). MIT licence.
#pragma once
#include <stdint.h>
#include <string>

namespace hxweb
{
struct Query {
    enum Kind : uint8_t { NONE, STATE, LOG, SEND, JOIN, BACKUP, RESTORE } kind = NONE;
    uint8_t channel = 0; // SEND: the channel and the text (UTF-8); JOIN: text is the #channel's name
    char text[168] = "";
    uint32_t contacts = 0; // the contacts version the page has (0: none): the list only comes again when it changed
    uint32_t packets = 0;  // the last packet number the page has
    uint32_t msgs = 0;     // the last message number
    uint32_t hist = 0;     // the end time of the history the page has
    // RESTORE: a backup file (NUL-terminated, in PSRAM), and which parts of it to bring back. The main loop owns it
    // once taken and frees it (free()) when done
    char *data = nullptr;
    size_t len = 0;
    enum Part : uint8_t { CONTACTS = 1, CHANNELS = 2, SETTINGS = 4, KEYS = 8 }; // KEYS: BACKUP with private channel keys
    uint8_t parts = 0;
};

bool start(uint16_t port); // true once serving (again a no-op)
void stop();
bool running();
void setPin(const char *pin); // six digits; a new PIN signs every browser out (and clears every wrong-PIN lock)
// the live feed's server, as set on the node: the page may open a WebSocket to it and to nowhere else (its CSP)
void setFeed(const char *url);
uint8_t sessions();           // browsers signed in now

// main loop: a page is waiting for data. Answer it on the same pass
bool take(Query &q);
void answer(std::string &&body);
} // namespace hxweb
