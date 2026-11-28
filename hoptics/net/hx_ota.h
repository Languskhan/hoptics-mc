// Hoptics-MC firmware updates from the web page, on trial: the new build starts in the other app slot and keeps
// itself only after a healthy minute; if it fails to start (or stops at a start-up error), the bootloader goes back
// to the build that was running. MIT licence.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hxota
{
// once at start, before anything slow: if this build came from a web update but a newer one has since been flashed
// over USB into the first slot (which the USB tools always write), starts that one instead
void bootCheck();
// the running build is on trial (a web update not yet kept)
bool trial();
// after a healthy minute: keeps the running build (no-op unless on trial)
void confirm();
// a start-up error on a build on trial: goes back to the previous build after a pause
void bootFailed();
// which app slot is running: "ota_0", "ota_1" or "factory"
const char *slot();

// one upload, fed in pieces by the web server; on failure `err` says why in plain words
struct Upload {
    void *handle = nullptr;
    size_t total = 0, got = 0;
    uint8_t head[64];
    size_t headLen = 0;
    char carry[24];
    size_t carryLen = 0;
    char board[16] = "";
    bool open = false;
};
bool begin(Upload &u, size_t total, const char *&err);
bool write(Upload &u, const uint8_t *data, size_t len, const char *&err);
bool finish(Upload &u, const char *&err); // checks it; it then waits for a tap on the node's own screen
void abort(Upload &u);
// a checked upload waits for someone at the node: the web PIN alone can't install firmware. The main loop asks on the
// screen, then calls accept() (starts it on the next restart) or decline() (keeps the build running now)
bool waiting();
unsigned waitingKB(); // its size, for the question
bool declined(); // the last upload was turned down on the screen, or nobody answered in time
bool failed();   // Install was tapped but the update couldn't be selected to start
bool accept(const char *&err);
void decline();
// set by accept(): the main loop restarts (after saving)
extern volatile bool restartWanted;
} // namespace hxota
