#pragma once
// airplay_receiver audio receiver orchestrator — PUBLIC header.
//
// C++ port of rbouteiller/airplay-esp32 main/audio/audio_receiver.h. This is the
// audio engine's orchestrator: it ties receive stream -> decrypt -> decode ->
// PCM ring -> playout timing together and exposes the upstream audio_receiver_*
// API so the RTSP transport and the audio output backend drive it.
//
// Shared contract:
//   * namespace esphome::airplay_receiver
//   * ALL heap via airplay_alloc/airplay_calloc/airplay_free (../allocator.h)
//   * logs via esphome/core/log.h + a per-file TAG
//   * upstream function signatures preserved
//
// DECRYPT (critical): audio_crypto.c is NOT ported. ChaCha20-Poly1305 decrypt
// goes through CryptoModule (crypto/crypto_module.h) and the upstream
// audio_encrypt_t is mapped to AudioEncrypt. The receiver owns the CryptoModule
// injection point (audio_receiver_set_crypto_module); the actual
// audio_crypto_decrypt_rtp / audio_crypto_decrypt_buffered call sites that were
// in audio_stream_realtime.c / audio_stream_buffered.c must become
// state->crypto->audio_decrypt_rtp(...) / audio_decrypt_buffered(...).
//
// The internal receiver state + stream helpers live in the private
// audio_receiver_internal.h (included by the stream agent's audio_stream.* in
// the same order as upstream, so the audio_stream/h sharing has no cycle).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "../crypto/crypto_module.h"  // AudioEncrypt (audio_encrypt_t mapping)

namespace esphome {
namespace airplay_receiver {

// ---------------------------------------------------------------------------
// Audio format info from ANNOUNCE SDP
// ---------------------------------------------------------------------------
typedef struct {
  char codec[32];      // "AppleLossless", "AAC", etc.
  int sample_rate;     // 44100, 48000, etc.
  int channels;        // 1 or 2
  int bits_per_sample; // 16, 24
  int frame_size;      // Samples per frame (ALAC: 352)

  // ALAC-specific config (from fmtp line)
  uint32_t max_samples_per_frame;
  uint8_t sample_size;
  uint8_t rice_history_mult;
  uint8_t rice_initial_history;
  uint8_t rice_limit;
  uint8_t num_channels;
  uint16_t max_run;
  uint32_t max_coded_frame_size;
  uint32_t avg_bit_rate;
  uint32_t sample_rate_config;
} audio_format_t;

// Audio buffer statistics
typedef struct {
  uint32_t packets_received;
  uint32_t packets_decoded;
  uint32_t packets_dropped;
  uint32_t decrypt_errors;
  uint32_t buffer_underruns;
  uint32_t buffer_overruns;
  uint32_t late_frames;
  uint16_t last_seq;
  uint32_t last_timestamp;
} audio_stats_t;

// Stream types for AirPlay 2
typedef enum {
  AUDIO_STREAM_NONE = 0,
  AUDIO_STREAM_REALTIME = 96,  // UDP, ALAC
  AUDIO_STREAM_BUFFERED = 103  // TCP, AAC-ELD
} audio_stream_type_t;

// Forward declaration so callers can refer to the stream type. The full
// definition lives in audio_stream.h (stream agent), pulled in via the private
// audio_receiver_internal.h.
struct audio_stream;
typedef struct audio_stream audio_stream_t;

// ---------------------------------------------------------------------------
// Public API (upstream audio_receiver_* preserved)
// ---------------------------------------------------------------------------

/** Initialize the audio receiver. */
esp_err_t audio_receiver_init(void);

/** Set audio format from ANNOUNCE SDP. */
void audio_receiver_set_format(const audio_format_t *format);

/**
 * Set encryption parameters for RTP decryption. Mapped to the component's
 * AudioEncrypt (ChaCha20-Poly1305 only); passing NULL clears encryption.
 */
void audio_receiver_set_encryption(const AudioEncrypt *encrypt);

/**
 * Inject the CryptoModule the audio pipeline decrypts through. The owner
 * (AirPlayReceiver) holds a CryptoModule member and passes it in here; the
 * stream tasks reach it via  state->crypto->audio_decrypt_rtp/buffered().
 */
void audio_receiver_set_crypto_module(CryptoModule *crypto);

/** Start receiving audio on the specified ports (active stream = REALTIME). */
esp_err_t audio_receiver_start(uint16_t data_port, uint16_t control_port);

/** Start the active stream type using the provided ports. */
esp_err_t audio_receiver_start_stream(uint16_t data_port, uint16_t control_port,
                                      uint16_t tcp_port);

/** Stop receiving audio. */
void audio_receiver_stop(void);

/** Get audio statistics. */
void audio_receiver_get_stats(audio_stats_t *stats);

/**
 * Read decoded PCM samples from the buffer.
 * @param buffer Output buffer for PCM samples (interleaved stereo, 16-bit)
 * @param samples Maximum number of samples to read (per channel)
 * @return Number of samples actually read
 */
size_t audio_receiver_read(int16_t *buffer, size_t samples);

/**
 * Scheduler-side diagnostics, named rather than numeric so a log line or an
 * HA entity can say WHY a board is quiet.
 *
 * `state` and `wait_reason` are the one pair that separates the failure modes:
 * WAIT_CLOCK_MAP means there is no usable clock (a PTP problem), while a
 * preroll/fallback reason means the clock is fine and the ring is not
 * playable.  Both are static strings owned by audio_scheduler.
 *
 * Before the engine exists, `engine_active` is false and the names read
 * "n/a" -- an idle board is not a stalled one, and the two must not look
 * alike.
 */
typedef struct {
  bool engine_active;
  bool playing;
  bool clock_map_valid;
  const char *state;
  const char *wait_reason;
  uint64_t conceal_events;
  uint64_t concealed_samples;
} audio_sched_diag_t;

void audio_receiver_get_sched_diag(audio_sched_diag_t *diag);

/**
 * Whether the samples from the last audio_receiver_read() were scheduler
 * silence rather than stream audio.
 *
 * The scheduler reports "nothing to play" by filling the caller's buffer with
 * zeros and returning the FULL sample count, not by returning 0 -- so a frame
 * count alone cannot distinguish a healthy stream from a wedged one (no
 * anchor, no clock map, paused). Anything that must tell those apart, such as
 * the output stage's idle amp power-down, has to ask.
 */
bool audio_receiver_last_read_was_silence(void);

/** Check if audio data is available. */
bool audio_receiver_has_data(void);

/** Flush the audio buffer (full stop path - TEARDOWN / stop). */
void audio_receiver_flush(void);

/** Flush the audio buffer for a mid-stream seek (FLUSH / FLUSHBUFFERED). */
void audio_receiver_seek_flush(void);

/** Arm a deferred flush for AirPlay 2 FLUSHBUFFERED with flushFromSeq. */
void audio_receiver_set_deferred_flush(uint32_t flush_until_ts);

/** Pause playback while preserving the timing anchor. */
void audio_receiver_pause(void);

/** Set the stream playout latency in samples. */
void audio_receiver_set_playout_latency_samples(uint32_t latency_samples);

/** Set advertised/target output latency in microseconds. */
void audio_receiver_set_output_latency_us(uint32_t latency_us);

/** Get current output latency in microseconds (buffer latency only). */
uint32_t audio_receiver_get_output_latency_us(void);

/** Get hardware output latency in microseconds (I2S DMA pipeline delay). */
uint32_t audio_receiver_get_hardware_latency_us(void);

/**
 * Get total latency in microseconds. DIAGNOSTIC ONLY - do NOT report this in
 * outputLatencyMicros (the RTSP layer advertises 0; see audio_timing.h).
 */
uint32_t audio_receiver_get_advertised_latency_us(void);

/** Provide anchor timing information from SETRATEANCHORTIME. */
void audio_receiver_set_anchor_time(uint64_t clock_id, uint64_t network_time_ns,
                                    uint32_t rtp_time);

/** Enable or pause playout scheduling. */
void audio_receiver_set_playing(bool playing);

/** Check if playback is currently active (not paused). */
bool audio_receiver_is_playing(void);

/** Reset the timing anchor (call when the PTP clock changes, e.g. SETPEERS). */
void audio_receiver_reset_timing(void);

/**
 * Set the client's control address for NACK retransmission requests.
 * @param client_ip Client IP in network byte order
 * @param client_control_port Client's control port (host byte order)
 */
void audio_receiver_set_client_control(uint32_t client_ip,
                                       uint16_t client_control_port);

/** Start the buffered audio receiver (type=103) on the TCP port. */
esp_err_t audio_receiver_start_buffered(uint16_t tcp_port);

/** Get the active stream port (data or buffered). */
uint16_t audio_receiver_get_stream_port(void);

/** Get the TCP port for buffered audio (after start_buffered). */
uint16_t audio_receiver_get_buffered_port(void);

/** Stop only the buffered receiver but keep playing buffered data. */
void audio_receiver_stop_buffered_only(void);

/** Set the stream type (realtime vs buffered). */
void audio_receiver_set_stream_type(audio_stream_type_t type);

}  // namespace airplay_receiver
}  // namespace esphome
