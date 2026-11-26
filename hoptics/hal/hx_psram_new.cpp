// Hoptics-MC: C++ objects (new) go to PSRAM. Internal RAM is what Wi-Fi, Bluetooth and the radio need, and on these
// boards little of it is left once Wi-Fi is on; the screens' many small objects (chat lines, map links) used to take
// it a few bytes at a time (the heap's own rule keeps blocks under 4 KB internal). C code (malloc) is unchanged.
// Internal RAM is still used when PSRAM is full or missing. MIT licence.
#if defined(HOPTICS_UI) && defined(ESP32)
#include <esp_heap_caps.h>
#include <new>
#include <stdlib.h>

namespace
{
void *take(size_t n)
{
    void *p = heap_caps_malloc_prefer(n ? n : 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}
} // namespace

void *operator new(size_t n)
{
    void *p = take(n);
    if (!p)
        abort(); // as the standard one does when there is no memory (exceptions are off)
    return p;
}
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return take(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return take(n); }
void operator delete(void *p) noexcept { heap_caps_free(p); }
void operator delete[](void *p) noexcept { heap_caps_free(p); }
void operator delete(void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete[](void *p, size_t) noexcept { heap_caps_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { heap_caps_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { heap_caps_free(p); }
#endif
