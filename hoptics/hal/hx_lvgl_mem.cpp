// Hoptics-MC: LVGL's memory comes from PSRAM when the board has it. MIT licence.
// The C library's malloc keeps every block under 4 KB in internal RAM, and LVGL's objects are all small, so the screens
// were eating the internal RAM that Bluetooth and WiFi need (the M9 sat at ~29 KB free). This prefers PSRAM for every
// LVGL allocation and falls back to internal RAM, so a board without PSRAM still works.
#include "lvgl.h"
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include <esp_heap_caps.h>

namespace
{
constexpr uint32_t PREFER = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, FALLBACK = MALLOC_CAP_DEFAULT;
}

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }
void *lv_malloc_core(size_t size) { return heap_caps_malloc_prefer(size, 2, PREFER, FALLBACK); }
void *lv_realloc_core(void *p, size_t new_size) { return heap_caps_realloc_prefer(p, new_size, 2, PREFER, FALLBACK); }
void lv_free_core(void *p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
