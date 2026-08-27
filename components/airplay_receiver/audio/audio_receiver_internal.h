#pragma once
// airplay_receiver audio receiver PRIVATE header (was audio_receiver_internal.h).
//
// Contains the receiver state and the stream glue declarations the audio_stream
// agent's audio_stream.{h,c} share. Include it AFTER audio_stream.h, exactly like
// upstream (audio_stream.c does #include "audio_receiver_internal.h"), so the
// full audio_stream_t definition is visible before the state struct and the
// inline audio_stream_state() helper are parsed — no include cycle.
//
// The CryptoModule* (crypto) field is the decrypt dependency: the owner injects
// it via audio_receiver_set_crypto_module(), and the stream task decrypt glue
// calls state->crypto->audio_decrypt_rtp(...) / audio_decrypt_buffered(...).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_receiver.h"

// FreeRTOS + lwIP socket types used by the receiver state. The real target is
// ESP-IDF; the fallbacks keep the header parseable for host static analysis.
#if defined(USE_ESP_IDF)
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
typedef void *TaskHandle_t;
typedef uint16_t in_port_t;
struct in_addr {
  uint32_t s_addr;
};
struct sockaddr_in {
  uint16_t sin_family;
  struct in_addr sin_addr;
  uint16_t sin_port;
};
#ifndef AF_INET
#define AF_INET 2
#endif
static inline uint16_t htons(uint16_t v) {
  return (uint16_t)((v << 8) | (v >> 8));
}
#endif

#include "audio_buffer.h"
#include "../decoder/audio_decoder.h"
#include "audio_stream.h"
#include "../timing/audio_timing.h"

namespace esphome {
namespace airplay_receiver {

#define MAX_RTP_PACKET_SIZE 2048

typedef struct {
  audio_stream_t *stream;
  audio_stream_t *realtime_stream;
  audio_stream_t *buffered_stream;

  audio_decoder_t *decoder;
  audio_buffer_t buffer;
  audio_timing_t timing;

  audio_stats_t stats;

  int data_socket;
  int control_socket;
  TaskHandle_t task_handle;
  TaskHandle_t control_task_handle;
  uint16_t data_port;
  uint16_t control_port;

  int buffered_listen_socket;
  int buffered_client_socket;
  uint16_t buffered_port;
  TaskHandle_t buffered_task_handle;
  uint8_t *buffered_recv_buffer;

  uint8_t *decrypt_buffer;
  size_t decrypt_buffer_size;

  uint64_t blocks_read;
  uint64_t blocks_read_in_sequence;

  // NACK retransmission support
  struct sockaddr_in client_control_addr;  // Client's control address for NACKs
  bool retransmit_enabled;                 // True when client address is set
  int64_t last_resend_error_time_us;       // Backoff timer on sendto failure
  bool rtp_sequence_valid;
  uint16_t resend_window_first;
  uint64_t resend_missing_mask;
  int64_t resend_last_request_time_us;

  // Post-seek RTP gates (logic in audio_receiver.cpp): a window
  // [discard_before_rtp, discard_above_rtp] around the new anchor. Frames
  // outside the window are dropped in audio_stream_accept_timestamp before they
  // enter the ring buffer.
  uint32_t discard_before_rtp;
  bool discard_before_rtp_valid;
  uint32_t discard_above_rtp;
  bool discard_above_rtp_valid;
  // Set by audio_receiver_seek_flush() to arm the gates on the next anchor.
  bool arm_gate_on_next_anchor;
  // Set by audio_receiver_seek_flush() to reject ALL incoming frames until the
  // next SETRATEANCHORTIME provides a valid anchor.
  bool discard_all_until_anchor;

  // Snapshot of the expected RTP position taken the moment the sender signals
  // PAUSE. Cleared on flush/reset and consumed after one use.
  uint32_t paused_rtp;
  bool paused_rtp_valid;

  // The CryptoModule the audio pipeline decrypts through. Injected by the owner
  // via audio_receiver_set_crypto_module(); the stream tasks decrypt with
  // state->crypto->audio_decrypt_rtp(...) / audio_decrypt_buffered(...).
  CryptoModule *crypto;
} audio_receiver_state_t;

// Lightweight RTP gate used by the buffered TCP task before decrypt/decode.
// Defined in audio_stream.c (stream agent).
bool audio_stream_accept_timestamp(audio_receiver_state_t *state,
                                   uint32_t timestamp);

// Decode and queue a frame whose timestamp has already passed the RTP gate.
bool audio_stream_process_accepted_frame(audio_receiver_state_t *state,
                                         uint32_t timestamp,
                                         const uint8_t *audio_data,
                                         size_t audio_len);

// Convenience entry point for realtime paths: gate, then decode and queue.
bool audio_stream_process_frame(audio_receiver_state_t *state,
                                uint32_t timestamp, const uint8_t *audio_data,
                                size_t audio_len);

static inline audio_receiver_state_t *audio_stream_state(audio_stream_t *stream) {
  return (audio_receiver_state_t *)stream->ctx;
}

}  // namespace airplay_receiver
}  // namespace esphome
