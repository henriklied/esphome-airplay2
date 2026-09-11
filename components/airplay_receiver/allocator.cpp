#include "allocator.h"

#include <cstdlib>  // malloc/calloc/free (non-ESP-IDF fallback path)

// ESP-IDF heap_caps_* memory-management. Guarded so the allocator still
// compiles (as a plain malloc/free shim) in non-ESP-IDF/static-analysis
// contexts that feed the file to a C++ compiler without the IDF headers.
#if defined(USE_ESP_IDF)
#include <esp_heap_caps.h>
#endif

// ---------------------------------------------------------------------------
// Platform memory-policy profile
// ---------------------------------------------------------------------------
// The values for every knob (AIRPLAY_RING_FRAMES, AIRPLAY_ALWAYS_INTERNAL_BYTES,
// AIRPLAY_INTERNAL_RESERVE_BYTES, per-task stacks, core affinity, decoder
// placement, BT enable) are carried by allocator.h, which is included above
// (via #include "allocator.h") and is in turn included by every module in the
// component. They are selected there by the AIRPLAY_PLATFORM_ESP32S3 /
// AIRPLAY_PLATFORM_ESP32 build flag emitted from __init__.py. The canonical
// reference remains platform/esp32s3/config.h and platform/esp32/config.h;
// allocator.h inlines the same values because ESPHome's external-component
// build does not copy the two-level platform/*/config.h headers into the
// generated tree.

// ---------------------------------------------------------------------------
// airplay_alloc
// ---------------------------------------------------------------------------
void *airplay_alloc(size_t size, bool realtime) {
  if (size == 0) {
    size = 1;
  }
#if defined(USE_ESP_IDF)
  if (realtime) {
    // Realtime: internal DRAM (fast, cacheable, DMA-safe).
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  // Non-realtime: small allocations stay internal (tuned threshold), larger
  // ones are PSRAM-first with an internal fallback -- the same policy as
  // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL + heap_caps_malloc_prefer().
  if (size <= AIRPLAY_ALWAYS_INTERNAL_BYTES) {
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  (void)realtime;
  return malloc(size);
#endif
}

// ---------------------------------------------------------------------------
// airplay_calloc
// ---------------------------------------------------------------------------
void *airplay_calloc(size_t n, size_t size, bool realtime) {
  if (n == 0 || size == 0) {
    n = 1;
    size = 1;
  }
#if defined(USE_ESP_IDF)
  if (realtime) {
    // Realtime: internal DRAM (fast, cacheable, DMA-safe). Note MALLOC_CAP_8BIT:
    // without it heap_caps_* may return 32-bit-only IRAM, which cannot be
    // written with 16-bit stores (the silence buffer is int16_t) and would
    // fault with a LoadStoreError.
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  // Mirror the airplay_alloc non-realtime policy: cache the threshold check on
  // the total byte count.
  size_t total = n * size;
  if (total <= AIRPLAY_ALWAYS_INTERNAL_BYTES) {
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  (void)realtime;
  return calloc(n, size);
#endif
}

// ---------------------------------------------------------------------------
// airplay_free
// ---------------------------------------------------------------------------
void airplay_free(void *ptr) {
  if (ptr == NULL) {
    return;
  }
#if defined(USE_ESP_IDF)
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

// ---------------------------------------------------------------------------
// Memory-pool reporters (diagnostics / heap tracing)
// ---------------------------------------------------------------------------
size_t airplay_internal_free(void) {
#if defined(USE_ESP_IDF)
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#else
  return 0;
#endif
}

size_t airplay_internal_largest_block(void) {
#if defined(USE_ESP_IDF)
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
#else
  return 0;
#endif
}

size_t airplay_psram_free(void) {
#if defined(USE_ESP_IDF)
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
  return 0;
#endif
}
