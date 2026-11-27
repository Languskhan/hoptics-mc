// Hoptics-MC microSD card. The card sits on an SPI bus it shares with the screen (and on the T-Deck and the M9 with the
// radio too), and that bus has one owner: the main loop (see hx_hal.h). So every card access runs on the main loop:
// work another task needs (the map's worker, the web server, the log) is handed over and done on the main loop's next
// pass, one file operation at a time, while that task waits (run()). No card, or a board without a slot: mounted() is
// false and nothing else changes. FAT32 cards (exFAT is not in this Arduino core). MIT licence.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

namespace hxsd
{
bool begin(void);   // main loop, once the screen is up (its driver starts the shared bus): mounts the card if one is in
bool mounted(void);
void tick(void);    // main loop, every pass: the card job another task is waiting on, if any
uint64_t totalBytes(void);
uint64_t freeBytes(void); // as of the last job (FAT keeps the count, so it costs nothing to read again)
uint32_t clusterBytes(void); // the smallest space a file takes

// fn(ctx) on the main loop with the card's bus free: at once on the main loop, otherwise on its next pass while this
// task waits (up to waitMs). False: no card, or the main loop didn't get to it in time (fn was not run)
bool run(void (*fn)(void *), void *ctx, uint32_t waitMs = 4000);
template <class F> bool run(F &&f, uint32_t waitMs = 4000)
{
    using T = typename std::remove_reference<F>::type;
    return run([](void *p) { (*static_cast<T *>(p))(); }, (void *)&f, waitMs);
}

// FatFS's own name for a path on the card ("0:/hoptics/logs"), for f_open() and friends: from inside run() only
void fatPath(char *out, size_t n, const char *path);
// whole-file helpers, from any task (each is one card job); folders on the way are made as needed
bool writeFile(const char *path, const void *data, size_t len, bool append = false);
int64_t fileSize(const char *path); // -1: not there
bool removeFile(const char *path);
bool renameFile(const char *from, const char *to); // replaces `to`
} // namespace hxsd
