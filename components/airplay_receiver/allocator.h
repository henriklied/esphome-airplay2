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

// ---------------------------------------------------------------------------
// Platform memory-policy profile
// ---------------------------------------------------------------------------
// The canonical values for every knob live in the platform profile headers
//   platform/esp32s3/config.h   (ESP32-S3 target, 8MiB octal PSRAM)
//   platform/esp32/config.h     (generic ESP32)
//
// ESPHome's external-component build copies only top-level (and one-level
// subdir) sources into the generated tree; the two-level platform/*/config.h
// headers are NOT copied, so the SAME values are inlined here as a
// self-contained, header-carried profile. This header (allocator.h) is
// included by every module in the component, so each translation unit sees the
// profile that matches the target and no consumer depends on the un-copied
// config.h. The active profile is selected by the AIRPLAY_PLATFORM_ESP32S3 /
// AIRPLAY_PLATFORM_ESP32 build flag emitted from the component's __init__.py
// (_add_memory_policy_flags). Keep the values below in lock-step with the
// corresponding platform header; when the component is compiled as a native
// ESP-IDF component (whole tree on the include path) the platform headers
// remain the canonical reference.
#if defined(AIRPLAY_PLATFORM_ESP32S3)
#ifndef AIRPLAY_RING_FRAMES
#define AIRPLAY_RING_FRAMES 1000
#endif
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 65536
#endif
#ifndef AIRPLAY_TASK_STACK_RTSP_CLIENT
#define AIRPLAY_TASK_STACK_RTSP_CLIENT 8192
#endif
#ifndef AIRPLAY_TASK_STACK_RTSP_SERVER
#define AIRPLAY_TASK_STACK_RTSP_SERVER 4096
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_RECV
#define AIRPLAY_TASK_STACK_AUDIO_RECV 12288
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_CTRL
#define AIRPLAY_TASK_STACK_AUDIO_CTRL 4096
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_BUFFERED
#define AIRPLAY_TASK_STACK_AUDIO_BUFFERED 4096
#endif
#ifndef AIRPLAY_TASK_STACK_PLAYBACK
#define AIRPLAY_TASK_STACK_PLAYBACK 4096
#endif
#ifndef AIRPLAY_TASK_STACK_NTP
#define AIRPLAY_TASK_STACK_NTP 3072
#endif
#ifndef AIRPLAY_CORE_AFFINITY_AUDIO
#define AIRPLAY_CORE_AFFINITY_AUDIO 1
#endif
#ifndef AIRPLAY_CORE_AFFINITY_RTSP
#define AIRPLAY_CORE_AFFINITY_RTSP 0
#endif
#ifndef AIRPLAY_DECODER_IN_PSRAM
#define AIRPLAY_DECODER_IN_PSRAM 1
#endif
#ifndef AIRPLAY_BT_ENABLED
#define AIRPLAY_BT_ENABLED 0
#endif
#elif defined(AIRPLAY_PLATFORM_ESP32)
#ifndef AIRPLAY_RING_FRAMES
#define AIRPLAY_RING_FRAMES 200
#endif
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 32768
#endif
#ifndef AIRPLAY_TASK_STACK_RTSP_CLIENT
#define AIRPLAY_TASK_STACK_RTSP_CLIENT 8192
#endif
#ifndef AIRPLAY_TASK_STACK_RTSP_SERVER
#define AIRPLAY_TASK_STACK_RTSP_SERVER 4096
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_RECV
#define AIRPLAY_TASK_STACK_AUDIO_RECV 12288
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_CTRL
#define AIRPLAY_TASK_STACK_AUDIO_CTRL 4096
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_BUFFERED
#define AIRPLAY_TASK_STACK_AUDIO_BUFFERED 4096
#endif
#ifndef AIRPLAY_TASK_STACK_PLAYBACK
#define AIRPLAY_TASK_STACK_PLAYBACK 4096
#endif
#ifndef AIRPLAY_TASK_STACK_NTP
#define AIRPLAY_TASK_STACK_NTP 3072
#endif
#ifndef AIRPLAY_CORE_AFFINITY_AUDIO
#define AIRPLAY_CORE_AFFINITY_AUDIO 1
#endif
#ifndef AIRPLAY_CORE_AFFINITY_RTSP
#define AIRPLAY_CORE_AFFINITY_RTSP 0
#endif
#ifndef AIRPLAY_DECODER_IN_PSRAM
#define AIRPLAY_DECODER_IN_PSRAM 1
#endif
#ifndef AIRPLAY_BT_ENABLED
#define AIRPLAY_BT_ENABLED 1
#endif
#else
// No/profile fallback: identical to the ESP32-S3 target so out-of-the-box
// behaviour matches the primary board.
#ifndef AIRPLAY_RING_FRAMES
#define AIRPLAY_RING_FRAMES 1000
#endif
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 65536
#endif
#ifndef AIRPLAY_TASK_STACK_RTSP_CLIENT
#define AIRPLAY_TASK_STACK_RTSP_CLIENT 8192
#endif
#ifndef AIRPLAY_TASK_STACK_RTSP_SERVER
#define AIRPLAY_TASK_STACK_RTSP_SERVER 4096
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_RECV
#define AIRPLAY_TASK_STACK_AUDIO_RECV 12288
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_CTRL
#define AIRPLAY_TASK_STACK_AUDIO_CTRL 4096
#endif
#ifndef AIRPLAY_TASK_STACK_AUDIO_BUFFERED
#define AIRPLAY_TASK_STACK_AUDIO_BUFFERED 4096
#endif
#ifndef AIRPLAY_TASK_STACK_PLAYBACK
#define AIRPLAY_TASK_STACK_PLAYBACK 4096
#endif
#ifndef AIRPLAY_TASK_STACK_NTP
#define AIRPLAY_TASK_STACK_NTP 3072
#endif
#ifndef AIRPLAY_CORE_AFFINITY_AUDIO
#define AIRPLAY_CORE_AFFINITY_AUDIO 1
#endif
#ifndef AIRPLAY_CORE_AFFINITY_RTSP
#define AIRPLAY_CORE_AFFINITY_RTSP 0
#endif
#ifndef AIRPLAY_DECODER_IN_PSRAM
#define AIRPLAY_DECODER_IN_PSRAM 1
#endif
#ifndef AIRPLAY_BT_ENABLED
#define AIRPLAY_BT_ENABLED 0
#endif
#endif

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
