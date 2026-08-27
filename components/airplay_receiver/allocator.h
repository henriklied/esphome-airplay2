#pragma once
// airplay_receiver centralized memory allocator.
//
// The AirPlay 2 receiver has two distinct allocation profiles:
//   * realtime (audio decode / DMA buffer / packet assembly) buffers that MUST
//     live in internal DRAM (cacheable, DMA-safe, no PSRAM flicker during
//     flash cache disable);
//   * non-realtime (PCM ring, socket payloads, metadata) buffers that may live
//     in PSRAM to spare internal RAM.
//
// Everything in the component allocates through this choke point so the
// memory policy is decided in one place (mirrors CONFIG_SPIRAM_MALLOC_ALWAYS-
// INTERNAL and heap_caps_malloc_prefer on ESP-IDF).

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate `size` bytes.
 *
 * @param realtime  true  -> internal DRAM (fast, cacheable, DMA-safe).
 *                  false -> PSRAM-first, with a small-allocation exception: any
 *                           request <= AIRPLAY_ALWAYS_INTERNAL_BYTES stays in
 *                           internal RAM regardless, mirroring
 *                           CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL.
 * @return  pointer to the allocation, or NULL on failure.
 */
void *airplay_alloc(size_t size, bool realtime);

/**
 * Calloc: allocate and zero `n` items of `size` bytes.
 *
 * @param realtime  same semantics as airplay_alloc().
 * @return  pointer to the zeroed allocation, or NULL on failure.
 */
void *airplay_calloc(size_t n, size_t size, bool realtime);

/** Free a pointer previously returned by airplay_alloc()/airplay_calloc(). */
void airplay_free(void *ptr);

/** Free internal DRAM capacity (heap_caps_get_free_size(MALLOC_CAP_INTERNAL)). */
size_t airplay_internal_free(void);

/** Largest contiguous free block in internal DRAM (bytes). */
size_t airplay_internal_largest_block(void);

/** Free PSRAM capacity (heap_caps_get_free_size(MALLOC_CAP_SPIRAM)). */
size_t airplay_psram_free(void);

#ifdef __cplusplus
}  // extern "C"
#endif
