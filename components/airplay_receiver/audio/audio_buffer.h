#pragma once
// airplay_receiver audio_buffer — decoder scratch space.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_buffer.{c,h} (PR #130)
// into the ESPHome component, wrapped in esphome::airplay_receiver. The
// upstream public API, struct layout and macros are preserved 1:1.
//
// This module used to own a 1000-slot PCM ring in PSRAM as well, sorted by RTP
// timestamp and consumed from the front.  Playback now runs entirely off the
// RTP-addressed timeline in audio_timeline.{c,h}, which stores decoded audio
// itself, so all that is left here is the staging area a decoder writes into
// before its PCM is pushed to the timeline.
//
// MEMORY POLICY (heap routes through ../allocator.h):
//   * `decode_buffer` is the decode working set — a fixed-size scratch area
//     written every frame by the ALAC/AAC decoders and read back by the
//     decoder agent before handoff to the timeline. It is a small, hot,
//     cacheable buffer so it lives in internal DRAM -> airplay_alloc(..., true).
//     (Upstream used plain malloc(); internal DRAM is the same placement.)
//   * No malloc/calloc/new/free anywhere; the buffer is released with
//     airplay_free() (which is NULL-safe).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace esphome {
namespace airplay_receiver {

#define AUDIO_MAX_CHANNELS     2
#define AUDIO_BYTES_PER_SAMPLE 2
/* Large enough for any frame either codec produces (AAC is 1024, ALAC 352). */
#define MAX_SAMPLES_PER_FRAME 4096

typedef struct {
  int16_t *decode_buffer;
  size_t decode_capacity_samples;
} audio_buffer_t;

esp_err_t audio_buffer_init(audio_buffer_t *buffer);
void audio_buffer_deinit(audio_buffer_t *buffer);
int16_t *audio_buffer_get_decode_buffer(audio_buffer_t *buffer,
                                        size_t *capacity_samples);

}  // namespace airplay_receiver
}  // namespace esphome
