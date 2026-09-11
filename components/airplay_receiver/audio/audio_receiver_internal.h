#pragma once
// airplay_receiver audio receiver PRIVATE header (was audio_receiver_internal.h).
//
// Contains the receiver state and the stream glue declarations the audio_stream
// agent's audio_stream.{h,cpp} share. Include it AFTER audio_stream.h exactly
// like upstream (audio_stream.c does #include "audio_receiver_internal.h"), so
// the full audio_stream_t definition is visible before the state struct and the
// inline audio_stream_state() helper are parsed - no include cycle.
//
// Engine v2 (PR #130): the receiver owns an audio_engine_v2_t value here (the
// engine agent's audio_engine_v2.h) as the singleton playback scheduler. The
// engine is created lazily on the first buffered SETUP (~790 KB PSRAM) and
// never torn down afterwards, because the playback task renders from it on
// every I2S refill. The decode worker + decoder mutex serialise stateful AAC
// decoding against audio_receiver_set_format()/stop().
//
// The CryptoModule* (crypto) field is the decrypt dependency: the owner injects
// it via audio_receiver_set_crypto_module(), and the stream task decrypt glue
// calls state->crypto->audio_decrypt_rtp(...) / audio_decrypt_buffered(...).
// Upstream field order is preserved; crypto is appended at the end as the
// port-only extension (audio_crypto.c is not ported).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_receiver.h"

// FreeRTOS + lwIP socket types used by the receiver state. The real target is
// ESP-IDF; the fallbacks keep the header parseable for host static analysis.
#if defined(USE_ESP_IDF)
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#else
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef uint16_t in_port_t;
// Host-parser fallbacks for the FreeRTOS semaphore calls used by
// audio_receiver.cpp (kept out of the ESP-IDF path, which uses real semphr.h).
inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return static_cast<void *>(nullptr); }
inline int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
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
#include "audio_engine_v2.h"      // audio_engine_v2_t + audio_timeline macros
#include "audio_decode_worker.h"  // audio_decode_worker_t
#include "audio_packet.h"         // audio_encoded_packet_t
#include "audio_stream.h"         // full audio_stream_t
#include "../decoder/audio_decoder.h"
#include "../timing/audio_timing.h"

namespace esphome {
namespace airplay_receiver {

#define MAX_RTP_PACKET_SIZE 2048

// Tagged struct (not anonymous): audio_decode_worker.h forward-declares
// `struct audio_receiver_state`, so the state type must carry that exact tag.
typedef struct audio_receiver_state {
  audio_stream_t *stream;
  audio_stream_t *realtime_stream;
  audio_stream_t *buffered_stream;

  audio_decoder_t *decoder;
  audio_buffer_t buffer;
  audio_timing_t timing;

  // AirPlay 2 buffered path only. Realtime (AirPlay 1) keeps using buffer +
  // timing above; the two are mutually exclusive at runtime.
  //
  // The engine owns ~790 KB of PSRAM, so it is created lazily on the first
  // buffered SETUP and then kept: tearing it down would race the playback task
  // that calls audio_engine_v2_render() on every I2S refill, and a device that
  // only ever serves AirPlay 1 never pays for it.
  audio_engine_v2_t engine_v2;
  bool engine_v2_ready;
  audio_decode_worker_t *decode_worker;
  // Serialises the stateful AAC decoder between the decode worker task and
  // audio_receiver_set_format()/stop(), which destroy and recreate it.
  SemaphoreHandle_t decoder_mutex;

  // AAC RTP continuity diagnostic for the buffered path: consecutive frames
  // must advance by exactly AUDIO_TIMELINE_FRAME_SAMPLES.
  uint32_t aac_diag_epoch;
  uint32_t aac_diag_last_rtp;
  bool aac_diag_rtp_valid;

  // Last SETRATEANCHORTIME, kept in the sender's PTP domain so it can be
  // re-armed once the PTP clock locks. An anchor that arrives while the offset
  // is still 0 maps to a wrapped RTP position and must not be used.
  bool engine_v2_anchor_pending;
  // Which clock the anchor above is expressed in, taken from the timeline ID
  // the sender supplied. PTP and NTP are unrelated absolute timelines, so
  // reading the offset from the other one puts the anchor decades away.
  bool engine_v2_anchor_uses_ptp;
  // Set on every audio_receiver_read(): true when the samples handed back were
  // scheduler silence rather than stream audio. See
  // audio_receiver_last_read_was_silence().
  bool last_read_was_silence;
  uint32_t engine_v2_anchor_rtp;
  uint64_t engine_v2_anchor_network_ns;
  int64_t engine_v2_playout_offset_ns;
  // Local time at which the current run of unpublished anchors began. The
  // sender re-anchors about once a second, so this is set when the run starts
  // and left alone afterwards; resetting it per anchor would race the
  // fallback deadline it exists to measure.
  int64_t engine_v2_anchor_pending_since_us;
  // The published anchor is the board's own clock, not the sender's, because
  // no network clock locked in time. Playback is unsynchronised with any other
  // receiver until a lock arrives and the real anchor replaces it.
  bool engine_v2_anchor_local_fallback;

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
  // Retransmission effectiveness, summarised once a second. Per-event logging
  // from the receive path is what this is avoiding: it runs on the audio core.
  uint32_t resend_sent_count;       // NACK requests actually sent
  uint32_t resend_recovered_count;  // retransmits that arrived in time
  uint32_t resend_stale_count;      // retransmits that arrived too late
  // Same two outcomes for packets that arrive with a backward sequence but no
  // retransmit payload type -- a resend the 0x56 check did not recognise.
  uint32_t resend_late_recovered_count;
  uint32_t resend_late_stale_count;
  int64_t resend_stats_log_us;
  // RX-path accounting, also once a second. lwIP counts udp.recv before the
  // socket's receive mbox, so the delta against packets_received is what was
  // dropped between the stack and this task -- loss the sequence-gap counters
  // cannot separate from loss on air. See rxpath_log_stats().
  uint32_t rxpath_prev_udp_recv;
  uint32_t rxpath_prev_packets_received;
  int64_t rxpath_log_us;

  // Post-seek RTP gates (logic in audio_receiver.cpp): a window
  // [discard_before_rtp, discard_above_rtp] around the new anchor. Frames
  // outside the window are dropped in audio_stream_accept_timestamp before they
  // enter the timeline.
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
// Returns false for frames that belong to the pre-seek/old-track backlog.
bool audio_stream_accept_timestamp(audio_receiver_state_t *state,
                                   uint32_t timestamp);

// Decode and queue a frame whose timestamp has already passed the RTP gate.
bool audio_stream_process_accepted_frame(audio_receiver_state_t *state,
                                         uint32_t timestamp,
                                         const uint8_t *audio_data,
                                         size_t audio_len);

// True when the next decoded AAC frame is a post-seek/resume priming frame and
// must be silenced. Read where the block counters are advanced, because the
// buffered path advances them on the TCP reader and decodes elsewhere.
bool audio_stream_aac_prime_mute_wanted(const audio_receiver_state_t *state);

// Buffered path: decode one encoded access unit and publish the PCM into the
// engine timeline. Runs on the decode worker task.
bool audio_stream_decode_encoded_packet(audio_receiver_state_t *state,
                                        const audio_encoded_packet_t *packet);

// Convenience entry point for realtime paths: gate, then decode and queue.
bool audio_stream_process_frame(audio_receiver_state_t *state,
                                uint32_t timestamp, const uint8_t *audio_data,
                                size_t audio_len);

static inline audio_receiver_state_t *audio_stream_state(audio_stream_t *stream) {
  return (audio_receiver_state_t *)stream->ctx;
}

}  // namespace airplay_receiver
}  // namespace esphome
