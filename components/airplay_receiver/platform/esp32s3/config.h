#pragma once
// airplay_receiver platform profile: ESP32-S3.
//
// This header is the canonical reference for the ESP32-S3 tuning. In the
// ESPHome external-component build only top-level (and one-level subdir)
// sources are copied, so the two-level platform/*/config.h headers are NOT
// copied into the generated tree; allocator.h therefore inlines the SAME
// values (selected by the AIRPLAY_PLATFORM_ESP32S3 / AIRPLAY_PLATFORM_ESP32
// build flag emitted from __init__.py). Keep this file in lock-step with
// allocator.h's AIRPLAY_PLATFORM_ESP32S3 branch.
//
// Target part: ESP32-S3-WROOM-1-N8R8 (8 MiB flash, 8 MiB octal PSRAM, 512 KiB
// internal SRAM). The ESP32-S3 has no Classic Bluetooth controller, so
// AIRPLAY_BT_ENABLED is 0 here even though the upstream audio path would use it
// on other chips.

// ---------------------------------------------------------------------------
// Audio ring buffer (PCM frame ring, stored in PSRAM)
// ---------------------------------------------------------------------------
// 1000 frames * (frame header + 352 samples * 2 ch * 2 bytes) ~= 1.42 MiB PCM
// ring. Matches upstream audio_buffer.h MAX_RING_BUFFER_FRAMES.
#ifndef AIRPLAY_RING_FRAMES
#define AIRPLAY_RING_FRAMES 1000
#endif

// ---------------------------------------------------------------------------
// Small-allocation threshold (mirrors CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
// ---------------------------------------------------------------------------
// Non-realtime allocations at or below this size stay in internal DRAM.
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif

// Internal DRAM reserved for the allocator's high-priority (realtime) uses.
// Software-kept headroom so DMA descriptors, WiFi management and the audio
// task stacks always have room even while the PCM ring fills PSRAM.
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 65536
#endif

// ---------------------------------------------------------------------------
// Per-task stack sizes (bytes). Values mirror the upstream task creation
// sites (rtsp_server.c, audio_stream_*.c, audio_output.c, ntp_clock.c).
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Core affinity (dual-core ESP32-S3)
// ---------------------------------------------------------------------------
// Audio (receive + control + playback) is pinned to core 1 so the RTSP/WiFi
// stack on core 0 never steals cycles from audio DMA filling.
#ifndef AIRPLAY_CORE_AFFINITY_AUDIO
#define AIRPLAY_CORE_AFFINITY_AUDIO 1
#endif
#ifndef AIRPLAY_CORE_AFFINITY_RTSP
#define AIRPLAY_CORE_AFFINITY_RTSP 0
#endif

// ---------------------------------------------------------------------------
// Decoder placement (tunable)
// ---------------------------------------------------------------------------
// 1 -> ALAC/AAC decode scratch buffers live in PSRAM (default for the big ring).
// 0 -> keep the decoder working set entirely in internal DRAM (more headroom
//      for realtime but less internal RAM free for other uses).
#ifndef AIRPLAY_DECODER_IN_PSRAM
#define AIRPLAY_DECODER_IN_PSRAM 1
#endif

// ---------------------------------------------------------------------------
// Bluetooth
// ---------------------------------------------------------------------------
// ESP32-S3 has no Classic BT controller -> disabled.
#ifndef AIRPLAY_BT_ENABLED
#define AIRPLAY_BT_ENABLED 0
#endif
