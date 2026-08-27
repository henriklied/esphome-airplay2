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
// The canonical values for every knob live in the platform profile headers
//   platform/esp32s3/config.h   (ESP32-S3 target, 8MiB octal PSRAM)
//   platform/esp32/config.h     (generic ESP32)
//
// ESPHome's external-component build copies only top-level (and one-level
// subdir) sources into the generated tree; the two-level platform/*/config.h
// headers are NOT copied, so the SAME profile values are inlined below to keep
// the generated allocator self-contained. The active profile is chosen by the
// AIRPLAY_PLATFORM_ESP32S3 / AIRPLAY_PLATFORM_ESP32 build flag emitted from
// the component's to_code(). Keep the values below in lock-step with the
// corresponding platform header. When the component is compiled as a native
// ESP-IDF component (whole tree on the include path) a future task may switch
// these to #include "platform/<variant>/config.h" instead.
#if defined(AIRPLAY_PLATFORM_ESP32S3)
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 65536
#endif
#elif defined(AIRPLAY_PLATFORM_ESP32)
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 32768
#endif
#else
// No/profile fallback: identical to the ESP32-S3 target so out-of-the-box
// behaviour matches the primary board.
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 65536
#endif
#endif

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
                                 MALLOC_CAP_INTERNAL);
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
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
  }
  // Mirror the airplay_alloc non-realtime policy: cache the threshold check on
  // the total byte count.
  size_t total = n * size;
  if (total <= AIRPLAY_ALWAYS_INTERNAL_BYTES) {
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
  }
  return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                 MALLOC_CAP_INTERNAL);
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
