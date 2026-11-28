#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
void *heap_caps_malloc(size_t, unsigned); size_t heap_caps_get_free_size(unsigned);
// The feed falls back to malloc() when heap_caps_malloc() fails. Routing that fallback through the stub lets the
// harness make both fail (FEED_NOMEM in stubs_impl.cpp). This header comes after <stdlib.h> in hx_feed.cpp.
void *hx_stub_malloc(size_t);
#ifndef HX_STUBS_IMPL
#define malloc(n) hx_stub_malloc(n)
#endif
