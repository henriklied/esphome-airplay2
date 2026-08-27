#pragma once
// airplay_receiver FreeRTOS audio decode worker (C++ port of
// rbouteiller/airplay-esp32 main/audio/audio_decode_worker.h).
//
// The decode worker is the background FreeRTOS task that pulls compressed, and
// ALREADY DECRYPTED, RTP access units out of the ingress queue and runs them
// through the decode + timeline stage.  It is deliberately split off the
// high-priority receive tasks so that bulk AAC/ALAC decoding never delays
// socket reads (RTP/control), and off the I2S playback task so it never delays
// a refill.
//
// SHARED CONTRACT (see audio_receiver.h / audio_stream.h):
//   * namespace esphome::airplay_receiver;
//   * ALL heap routed through ../allocator.h (airplay_alloc / airplay_free) —
//     no malloc / calloc / heap_caps_* directly;
//   * diagnostics via ESP_LOGx (esphome/core/log.h) with a file-local TAG;
//   * upstream signatures preserved 1:1.
//
// DECRYPT / DECODE / TIMELINE (the pipeline this worker participates in):
//   * DECRYPT — done by the ingress (audio_stream_realtime.cpp /
//     audio_stream_buffered.cpp) via state->crypto->audio_decrypt_rtp(...) /
//     audio_decrypt_buffered(...), BEFORE the access unit is enqueued here.
//     Each job therefore carries a plaintext payload (audio_encoded_packet_t).
//   * DECODE   — audio_decoder_decode(state->decoder, ...) inside
//     audio_stream_decode_encoded_packet() (audio_stream.cpp, stream agent).
//   * TIMELINE — audio_engine_v2_deferred_flush() +
//     audio_engine_v2_push_pcm_wait() inside audio_stream_decode_encoded_packet().
//     This worker calls audio_stream_decode_encoded_packet(state, &packet) per
//     job; the helper owns the decode + timeline push behind the decoder mutex.
//
// The receiver state (audio_receiver_state_t) is forward-declared here and
// defined in audio_receiver_internal.h; that header includes this one, so we
// must NOT include it from here or we create a cycle.  Callers pass the
// typedef-compatible pointer freely.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_packet.h"  // audio_encoded_packet_t

namespace esphome {
namespace airplay_receiver {

// Forward declare the tagged receiver state.  audio_receiver_internal.h defines
// `typedef struct audio_receiver_state { ... } audio_receiver_state_t;` and
// includes this header, so the two typedefs agree (cycle-safe).
typedef struct audio_receiver_state audio_receiver_state_t;

/// Opaque worker handle.  Defined privately in audio_decode_worker.cpp.
typedef struct audio_decode_worker audio_decode_worker_t;

/// Create the decode worker + its FreeRTOS task for `state`.
///
/// The worker owns a pointer-job queue (depth AUDIO_DECODE_QUEUE_DEPTH) that it
/// drains on a dedicated task.  On success `*out_worker` is set and ESP_OK is
/// returned; on failure an ESP error is returned and *out_worker is untouched.
esp_err_t audio_decode_worker_create(audio_receiver_state_t *state,
                                     audio_decode_worker_t **out_worker);

/// Destroy the worker and stop/detach its task.  Safe on a NULL or already
/// torn-down worker.
void audio_decode_worker_destroy(audio_decode_worker_t *worker);

/// Result of audio_decode_worker_enqueue().
typedef enum {
  AUDIO_DECODE_ENQUEUE_OK = 0,     // Job accepted onto the queue.
  AUDIO_DECODE_ENQUEUE_RETRY,      // Queue full; caller should retry.
  AUDIO_DECODE_ENQUEUE_DROP,       // Invalid / epoch-stale / alloc failure.
} audio_decode_enqueue_result_t;

/// Copy `*packet` (deep-copying its payload) and enqueue it for decode.
///
/// The payload must already be decrypted (see the DECRYPT note above).  The job
/// is rejected unless it matches the current engine epoch, so a flush invalidates
/// every in-flight and queued copy without per-slot accounting.
audio_decode_enqueue_result_t audio_decode_worker_enqueue(
    audio_decode_worker_t *worker, const audio_encoded_packet_t *packet,
    uint32_t timeout_ms);

/// Invalidate any in-flight and queued jobs (used by seek / FLUSH / TEARDOWN).
void audio_decode_worker_discard_pending(audio_decode_worker_t *worker);

/// Number of jobs currently sitting in the queue (0 on NULL worker).
size_t audio_decode_worker_pending(const audio_decode_worker_t *worker);

/// True when the queue has fewer than 4 free slots (back-pressure hint).
bool audio_decode_worker_is_nearly_full(const audio_decode_worker_t *worker);

}  // namespace airplay_receiver
}  // namespace esphome
