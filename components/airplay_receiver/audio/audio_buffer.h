#pragma once
// airplay_receiver audio_buffer — sorted, RTP-ordered PCM ring buffer.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_buffer.{c,h} into the
// ESPHome component. The upstream public API is preserved 1:1 (function
// names, parameter lists and struct layouts are unchanged) and the whole
// module is wrapped in esphome::airplay_receiver so it links cleanly beside
// the other ported slices.
//
// MEMORY POLICY (all heap routes through ../allocator.h):
//   * the PCM pool (`pool`) lives in PSRAM  -> airplay_alloc(..., realtime=false).
//     It is a large, low-bandwidth ring buffer (capacity * BYTES_PER_FRAME
//     ~= 1.4 MiB on this part); it must NOT occupy internal DRAM.
//   * the slot index arrays (`sorted`, `free_stack`) and the decode scratch
//     buffer (`frame_buffer`) stay in internal DRAM -> realtime=true.
//     They are small, are touched inside a spinlock, and form the
//     decode/DMA working set, matching upstream's malloc() (internal) placement.
//   * no malloc/calloc/new/free anywhere; every buffer is released with
//     airplay_free() (which is NULL-safe).
//
// `audio_stats_t` is defined here (its upstream canonical home is
// main/audio/audio_receiver.h) so this module is self-contained and
// compilable on its own. When audio_receiver.h is ported into the component,
// switch this header to `#include "audio_receiver.h"` and drop this local
// definition so the struct has exactly one definition in each TU (ODR).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "audio_receiver.h"  // audio_stats_t + audio_format_t (single source of truth)

namespace esphome {
namespace airplay_receiver {

#define AAC_FRAMES_PER_PACKET  352
#define AUDIO_MAX_CHANNELS     2
#define AUDIO_BYTES_PER_SAMPLE 2
#define MAX_SAMPLES_PER_FRAME  4096

struct __attribute__((packed)) audio_frame_header_t {
  uint32_t rtp_timestamp;
  uint16_t samples_per_channel;
  uint8_t channels;
  uint8_t reserved;
};

#define MAX_RING_BUFFER_FRAMES 1000
#define BYTES_PER_FRAME                                          \
  ((size_t) sizeof(audio_frame_header_t) +                       \
   ((size_t) AAC_FRAMES_PER_PACKET * (size_t) AUDIO_MAX_CHANNELS * \
    (size_t) AUDIO_BYTES_PER_SAMPLE))
#define AUDIO_BUFFER_SIZE (MAX_RING_BUFFER_FRAMES * BYTES_PER_FRAME)

/// RTP-ordered PCM ring buffer managed through the airplay_* allocator.
struct audio_buffer_t {
  uint8_t *pool;                // Pre-allocated frame data in PSRAM
  uint16_t *sorted;             // Slot indices sorted by RTP timestamp
  uint16_t *free_stack;         // Stack of free slot indices
  int count;                    // Frames currently in buffer
  int free_top;                 // Top of free stack (next free slot)
  int capacity;                 // Max frames
  size_t slot_size;             // BYTES_PER_FRAME
  portMUX_TYPE lock;            // Spinlock for count/index manipulation
  SemaphoreHandle_t data_ready; // Counting semaphore (blocks consumer)
  uint8_t *frame_buffer;        // Temp assembly buffer
  int16_t *decode_buffer;       // Decode buffer pointer
  size_t decode_capacity_samples;
};

/// Allocate the PSRAM pool + internal index/scratch arrays, create the
/// counting semaphore, and return a ready-to-use buffer. Caller owns the
/// `buffer` struct storage.
esp_err_t audio_buffer_init(audio_buffer_t *buffer);
/// Release every airplay_* allocation (NULL-safe per-field) and reset state.
void audio_buffer_deinit(audio_buffer_t *buffer);
/// Return all queued slots to the free stack and drain the semaphore.
void audio_buffer_flush(audio_buffer_t *buffer);
/// Number of frames currently queued (diagnostics / back-pressure).
int audio_buffer_get_frame_count(audio_buffer_t *buffer);
bool audio_buffer_is_nearly_full(audio_buffer_t *buffer);
/// Newest (highest-RTP) frame currently queued. Diagnostics only.
bool audio_buffer_peek_newest_rtp(audio_buffer_t *buffer, uint32_t *rtp_out);
/// First frame of the contiguous run that ends at the newest frame
/// (skips stale islands stranded below the real stream head).
bool audio_buffer_bulk_start_rtp(audio_buffer_t *buffer, uint32_t *rtp_out);
/// Block (up to `ticks`) for the oldest frame; returns a frame pointer in
/// `*item` + its byte size in `*item_size`. Caller must call return().
bool audio_buffer_take(audio_buffer_t *buffer, void **item, size_t *item_size,
                       TickType_t ticks);
/// Return a frame previously obtained via take() to the free stack.
void audio_buffer_return(audio_buffer_t *buffer, void *item);
/// Decode scratch buffer (internal DRAM); `capacity_samples` is set on return.
int16_t *audio_buffer_get_decode_buffer(audio_buffer_t *buffer,
                                        size_t *capacity_samples);
/// Queue decoded PCM, splitting large frames into AAC_FRAMES_PER_PACKET chunks.
bool audio_buffer_queue_decoded(audio_buffer_t *buffer, audio_stats_t *stats,
                                uint32_t timestamp, const int16_t *pcm_data,
                                size_t samples, int channels);
/// Peek the RTP timestamp of the oldest (lowest-timestamp) frame without
/// removing it. Returns false if the buffer is empty.
bool audio_buffer_oldest_timestamp(audio_buffer_t *buffer, uint32_t *timestamp);

}  // namespace airplay_receiver
}  // namespace esphome
