#pragma once
// airplay_receiver platform profile: generic ESP32 (Classic / WROVER family).
//
// Conservative defaults for the smaller internal DRAM heap and typically
// 4 MiB PSRAM boards. Same single-source-of-truth note as platform/esp32s3/
// config.h: allocator.h inlines these values for the ESPHome build (selected by
// the AIRPLAY_PLATFORM_ESP32S3 / AIRPLAY_PLATFORM_ESP32 flag); keep in
// lock-step.

// ---------------------------------------------------------------------------
// Audio ring buffer (PCM frame ring, stored in PSRAM)
// ---------------------------------------------------------------------------
// Smaller than the S3 target: generic ESP32 boards usually pair 4 MiB PSRAM
// with a ~160 KiB internal heap, so a smaller ring leaves PSRAM for the app.
#ifndef AIRPLAY_RING_FRAMES
#define AIRPLAY_RING_FRAMES 200
#endif

// ---------------------------------------------------------------------------
// Small-allocation threshold (mirrors CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
// ---------------------------------------------------------------------------
#ifndef AIRPLAY_ALWAYS_INTERNAL_BYTES
#define AIRPLAY_ALWAYS_INTERNAL_BYTES 1024
#endif

// Internal DRAM headroom reserved for realtime/high-priority allocations.
#ifndef AIRPLAY_INTERNAL_RESERVE_BYTES
#define AIRPLAY_INTERNAL_RESERVE_BYTES 32768
#endif

// ---------------------------------------------------------------------------
// Per-task stack sizes (bytes) -- same task set as ESP32-S3.
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
// Core affinity (dual-core ESP32)
// ---------------------------------------------------------------------------
#ifndef AIRPLAY_CORE_AFFINITY_AUDIO
#define AIRPLAY_CORE_AFFINITY_AUDIO 1
#endif
#ifndef AIRPLAY_CORE_AFFINITY_RTSP
#define AIRPLAY_CORE_AFFINITY_RTSP 0
#endif

// ---------------------------------------------------------------------------
// Decoder placement (tunable)
// ---------------------------------------------------------------------------
#ifndef AIRPLAY_DECODER_IN_PSRAM
#define AIRPLAY_DECODER_IN_PSRAM 1
#endif

// ---------------------------------------------------------------------------
// Bluetooth
// ---------------------------------------------------------------------------
// Generic ESP32 has a Classic / BLE controller -> enabled by default.
#ifndef AIRPLAY_BT_ENABLED
#define AIRPLAY_BT_ENABLED 1
#endif
