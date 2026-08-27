#pragma once
// airplay_receiver playout-timing engine.
//
// Port of upstream airplay-esp32 main/audio/audio_timing.{c,h} into isolated
// C++ under esphome::airplay_receiver.  The engine maps an AirPlay RTP
// timestamp onto a wall-clock anchor (from SETRATEANCHORTIME, delivered via
// audio_timing_set_anchor()), computes each frame's scheduled playout moment
// (early_us), and releases decoded PCM to the output stage on that schedule,
// modelling the hardware pipeline delay through audio_output.
//
// Heap is routed through airplay_* (../allocator.h): the only allocation is
// the single pending early-frame buffer.  Diagnostics go through ESP_LOGx via
// esphome/core/log.h with TAG "audio_time".
//
// Dependencies (provided by sibling slices, not owned here):
//   * audio/audio_output.h      — audio_output_get_pipeline_us(),
//                                 audio_output_get_hardware_latency_us(),
//                                 audio_output_get_underruns().
//   * timing/ptp_clock.h        — ptp_clock_get_offset_ns(),
//                                 ptp_clock_is_locked(), ptp_clock_get_stats()
//                                 + ptp_stats_t (AirPlay 2 sync mode).
//   * timing/ntp_clock.h        — ntp_clock_get_offset_ns(),
//                                 ntp_clock_is_locked() (AirPlay 1 sync mode).
//   * audio/audio_buffer.h / audio/audio_stream.h / audio/audio_receiver.h —
//                                 the decoded-frame ring, the stream descriptor
//                                 and the format/statistics structs.

#include <cstddef>
#include <cstdint>

#include "../audio/audio_buffer.h"
#include "../audio/audio_receiver.h"
#include "../audio/audio_stream.h"

namespace esphome {
namespace airplay_receiver {

typedef struct {
  uint32_t output_latency_us;
  uint32_t target_buffer_frames;
  uint32_t nominal_frame_samples;
  bool playout_started;
  bool playing;
  bool anchor_valid;
  uint64_t anchor_network_time_ns;
  uint32_t anchor_rtp_time;
  int64_t anchor_local_time_ns;
  int64_t ready_time_us;  // When buffer became ready (0 = not ready yet)
  bool ptp_locked;
  uint8_t *pending_frame;
  size_t pending_frame_len;
  size_t pending_frame_capacity;
  bool pending_valid;
  // Early-frame guard: counts consecutive early frames to detect a stuck
  // anchor. Reset whenever a new anchor is set or a late/on-time frame is
  // played.
  int consecutive_early_frames;
  // Stream playout latency in samples, added to every frame's scheduled
  // play time.  Realtime streams (type 96): the anchor maps RTP onto the
  // sender's source timeline and playout happens latencyMin samples later
  // (11025 = 250 ms unless SETUP says otherwise).  Buffered streams
  // (type 103): 0 — the anchor is the play time.  Set at stream SETUP;
  // survives audio_timing_reset() because it is stream configuration, not
  // playback state.
  uint32_t playout_latency_samples;
  // Counts played frames so the periodic playout report can be rate-limited.
  uint32_t playout_reports;
  // RTP continuity tracking.  expected_rtp is the timestamp the NEXT frame
  // must carry to be contiguous with what was just played.  A fresh frame
  // above it means packets were lost and never recovered: the playout gap is
  // concealed with schedule-length silence instead of skipping ahead (which
  // would both pop and shift the whole playback position early).  gaps
  // counts concealed discontinuities (diagnostics).
  uint32_t expected_rtp;
  bool expected_rtp_valid;
  uint32_t gaps;
  // Rate limiting for the gap-conceal warning logs.  Logging is blocking
  // (UART + ring mutex), so unthrottled per-frame warnings slow the
  // late-frame drain loop to roughly realtime — turning a stream change
  // with a deep stale buffer into seconds of stalled audio.
  int64_t last_gap_log_us;
  uint32_t gaps_suppressed;
  // Position servo state (see POS_SERVO_* in audio_timing.cpp).
  // pos_err_filtered_us: IIR-smoothed playout position error.
  // servo_engaged/servo_phase: hysteresis state and trim rate divider.
  // servo_trims: total single-sample corrections applied (diagnostics).
  int64_t pos_err_filtered_us;
  bool servo_engaged;
  uint8_t servo_phase;
  uint32_t servo_trims;
  // Quick-start flag: set after a seek/flush/track-change so that
  // audio_timing_read starts playback with just 1 buffered frame instead of
  // waiting for target_buffer_frames.  Anchor-based timing is used from the
  // very first frame (no bypass) — early frames are held as pending and
  // silence is output until their scheduled play time, exactly like
  // shairport-sync.  Cleared once playout_started becomes true.
  bool quick_start;
  // Deferred flush (AirPlay 2 FLUSHBUFFERED with flushFromSeq present):
  // keep playing until a frame with rtp_timestamp >= flush_until_ts arrives,
  // then bulk-flush and start fresh.  Written by the RTSP task, read by the
  // DMA callback task.  Aligned 32-bit + bool — atomic on Xtensa without a
  // mutex (write flush_until_ts first, arm bool second; read bool first).
  bool deferred_flush_pending;
  uint32_t flush_until_ts;

  // Persistent statistics for a late-frame drain episode.  Kept in the timing
  // state so repeated playout callbacks produce one summary log instead of
  // one UART write per dropped frame.
  uint32_t late_drop_count;
  bool late_drop_active;
} audio_timing_t;

void audio_timing_init(audio_timing_t *timing, size_t pending_capacity);
void audio_timing_reset(audio_timing_t *timing);
// Clear playback-derived continuity + servo filter state. Must run on every
// re-lock (flush/seek/track-change), else a stale expected_rtp or servo bias
// survives into the new segment.
void audio_timing_reset_continuity(audio_timing_t *timing);
void audio_timing_set_format(audio_timing_t *timing, const audio_format_t *format);
void audio_timing_set_output_latency(audio_timing_t *timing, const audio_format_t *format,
                                     uint32_t latency_us);
uint32_t audio_timing_get_output_latency(const audio_timing_t *timing);
uint32_t audio_timing_get_hardware_latency(void);
// Total end-to-end latency (output target + HW DMA + fixed pipeline delay).
//
// DIAGNOSTIC ONLY — do NOT wire this into outputLatencyMicros.
// rtsp_handlers.c deliberately advertises 0 for both inputLatencyMicros and
// outputLatencyMicros because compute_early_us() already compensates for the
// hardware and pipeline delay internally; advertising a non-zero value makes
// the sender adjust its anchor as well and the delay is applied twice.
// shairport-sync likewise advertises no audioLatencies.
//
// Note also that this figure does not include the sender-driven pre-buffer
// actually sitting in the jitter buffer during playback (frequently 1.5 s+),
// so it is not the true end-to-end delay either. Use it for logging and
// introspection, not for protocol negotiation.
uint32_t audio_timing_get_advertised_latency(const audio_timing_t *timing);
// Set the stream playout latency (samples).  See playout_latency_samples.
void audio_timing_set_playout_latency(audio_timing_t *timing, uint32_t latency_samples);
void audio_timing_set_anchor(audio_timing_t *timing, const audio_format_t *format,
                             uint64_t clock_id, uint64_t network_time_ns, uint32_t rtp_time);
void audio_timing_set_playing(audio_timing_t *timing, bool playing);
size_t audio_timing_read(audio_timing_t *timing, audio_buffer_t *buffer,
                         const audio_stream_t *stream, audio_stats_t *stats, int16_t *out,
                         size_t samples);

}  // namespace airplay_receiver
}  // namespace esphome
