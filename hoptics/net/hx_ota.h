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
bool finish(Upload &u, const char *&err); // checks it, and starts it on the next restart
void abort(Upload &u);
// set by a finished upload: the main loop restarts (after saving) once the page has its answer
extern volatile bool restartWanted;
} // namespace hxota
