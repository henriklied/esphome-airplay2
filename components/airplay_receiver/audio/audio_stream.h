#pragma once
// airplay_receiver audio_stream base (C++ port of rbouteiller/airplay-esp32
// main/audio/audio_stream.h, engine-v2 / PR #130).
//
// Defines the audio_stream container + its ops vtable and the upstream
// audio_stream_* lifecycle API. The realtime (UDP, type 96) and buffered
// (TCP, type 103) receiver slices each provide an audio_stream_ops_t table;
// this base file owns the container creation/destruction and the shared
// RTP-gate + decode entry points that both paths run through.
//
// SHARED CONTRACT (also documented in audio_receiver.h):
//   * namespace esphome::airplay_receiver
//   * ALL heap via airplay_alloc/airplay_calloc/airplay_free (../allocator.h)
//   * logs via esphome/core/log.h + a per-file TAG
//   * upstream function signatures / struct layouts preserved
//
// TYPE SOURCES: audio_stream_type_t / audio_format_t / audio_stats_t live in
// audio_receiver.h (the public receiver header), which this header pulls in via
// #include (it forward-declares `struct audio_stream` and typedefs
// audio_stream_t). The private receiver state + audio_stream_state() + the
// shared RTP-gate / decode / decode-worker-callback declarations live in
// audio_receiver_internal.h (owned by the receiver agent), included by
// audio_stream.cpp / audio_stream_realtime.cpp / audio_stream_buffered.cpp
// AFTER this header (matching upstream order) so the full audio_stream_t
// definition is visible before the state struct is parsed.
//
// DECRYPT (engine-v2): upstream audio_crypto.c is NOT ported. Per-stream
// encryption is configured with the component AudioEncrypt (ChaCha20-Poly1305
// only) and actual decrypt goes through the injected CryptoModule
// (crypto/crypto_module.h), reached via state->crypto->audio_decrypt_rtp /
// audio_decrypt_buffered — see audio_stream_realtime.cpp / buffered.cpp.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "audio_receiver.h"

namespace esphome {
namespace airplay_receiver {

// audio_receiver.h already forward-declares `struct audio_stream` and typedefs
// `audio_stream_t`; the full definition is provided below.

// Operations vtable for a concrete stream implementation (realtime vs buffered).
// receive_packet / decrypt_payload are unused by both engine-v2 receiver paths
// (their receive loops are self-contained FreeRTOS tasks); they are kept for
// API symmetry with upstream audio_stream_ops_t.
typedef struct {
  esp_err_t (*start)(audio_stream_t *stream, uint16_t port);
  void (*stop)(audio_stream_t *stream);
  bool (*receive_packet)(audio_stream_t *stream);
  int (*decrypt_payload)(audio_stream_t *stream, const uint8_t *in,
                         size_t in_len, uint8_t *out, size_t out_cap);
  uint16_t (*get_port)(audio_stream_t *stream);
  bool (*is_running)(audio_stream_t *stream);
  void (*destroy)(audio_stream_t *stream);
} audio_stream_ops_t;

struct audio_stream {
  const audio_stream_ops_t *ops;
  audio_stream_type_t type;
  bool running;
  audio_format_t format;
  AudioEncrypt encrypt;  // component AudioEncrypt (ChaCha20-Poly1305 only)
  void *ctx;             // audio_receiver_state_t * (see audio_stream_state())
};

// ---------------------------------------------------------------------------
// Public lifecycle (upstream audio_stream_* preserved)
// ---------------------------------------------------------------------------
audio_stream_t *audio_stream_create_realtime(void);
audio_stream_t *audio_stream_create_buffered(void);
void audio_stream_destroy(audio_stream_t *stream);
bool audio_stream_uses_buffer(audio_stream_type_t type);

// Ops tables supplied by the concrete slices (audio_stream_realtime.cpp /
// audio_stream_buffered.cpp). Declared extern so the functions in
// audio_stream.cpp can resolve them with external linkage.
extern const audio_stream_ops_t audio_stream_realtime_ops;
extern const audio_stream_ops_t audio_stream_buffered_ops;

}  // namespace airplay_receiver
}  // namespace esphome
