// airplay_receiver playout-timing engine (port of upstream
// airplay-esp32 main/audio/audio_timing.c).
//
// Maps an AirPlay RTP timestamp onto a wall-clock anchor (SETRATEANCHORTIME),
// computes each frame's scheduled playout moment (early_us), and releases
// decoded PCM on that schedule, modelling the hardware pipeline delay through
// audio_output.  Heap is routed through airplay_*; diagnostics via ESP_LOGx.
#include "audio_timing.h"

#include <cinttypes>
#include <cstdlib>
#include <cstring>

#include "esphome/core/log.h"
#include "esp_timer.h"

#include "../allocator.h"
#include "../audio/audio_output.h"
#include "ntp_clock.h"
#include "ptp_clock.h"

#define DEFAULT_BUFFER_LATENCY_US 2000  // 2ms startup jitter buffer
// Additional pipeline latency to account for task scheduling, I2S write
// blocking, and resampler processing.  Without this, frames pass the
// timing check "on time" but actually exit the speaker several ms later.
#define PIPELINE_LATENCY_US 5000  // ~5ms scheduling + write delay
#define MIN_STARTUP_FRAMES 4

// Position servo: corrects small standing playout offsets by trimming or
// duplicating single samples.  Residual position errors arise from
// drop-recovery after network stalls (bounded by the late threshold) and
// from crystal drift accumulating between sender and local clock (~10-40
// ppm, i.e. 40-140 ms/hour) — inside the early/late gate's dead zone nothing
// else corrects position, so without this a 20 ms post-stall bias persists
// for the rest of the session (observed on hardware).
//
// Control law, and why the earlier (removed) servo failed: that design
// compared the ABSOLUTE error against a deadband and "credited" each trim
// back onto the filtered error.  Against the then-undiagnosed 250 ms
// structural offset the credit was ~1000x weaker than the filter's
// re-tracking, so it pinned at max authority — a continuous 0.28% stretch.
// This design has no credit term (trims change reality; the filter simply
// tracks it), engages only outside POS_SERVO_ENGAGE_US with hysteresis, and
// is rate-limited to one sample per POS_SERVO_TRIM_INTERVAL frames:
//   1 / (352 x 4) = 710 ppm = 0.07% pitch deviation (inaudible; JND ~0.2%).
// Saturation is impossible by construction: the early/late gate bounds any
// played frame's error to +/-threshold (50 ms realtime), and 50 ms at
// 710 ppm converges in ~70 s, after which the servo disengages.
#define POS_SERVO_FILTER_DIV 16   // IIR divisor, ~0.13 s time constant
#define POS_SERVO_ENGAGE_US 5000  // engage when |filtered err| exceeds this
#define POS_SERVO_DISENGAGE_US 1500  // disengage when it falls below this
#define POS_SERVO_TRIM_INTERVAL 4    // one 1-sample trim per this many frames
// Innovation clamp: cap how far one frame's measurement can move the filter.
// The per-frame error measurement is NOISY in a one-sided way: the DMA ring
// holds ~40 ms of queued audio, so when the playback task is briefly starved
// (WiFi, metadata bursts) the READ happens late and the measurement says
// "late" — but the audio on the wire never moved; the ring absorbed the
// delay.  These artifacts are always negative (a delayed read can only
// measure late), so an unclamped average is dragged below the true position
// and the servo chases offsets that do not exist.  Observed on hardware:
// engage/disengage chatter every few seconds with apparent inter-cycle
// "drift" of 750-1650 ppm — 20-60x anything a crystal can do — alongside
// single-frame err spikes of -22 ms next to -2 ms readings.  With the clamp,
// a -22 ms spike moves the filter by at most CLAMP/DIV ~= 94 us, so spike
// clusters cannot reach the engage threshold, while a REAL standing offset
// (present on every frame) still walks the filter to engagement in ~0.5 s.
#define POS_SERVO_INNOV_CLAMP_US 1500

// Every audio output backend (I2S, S/PDIF, USB) allocates its read buffer as
// (FRAME_SAMPLES + 1) * 2 int16 samples — i.e. interleaved stereo — and the
// output stage is stereo regardless of what an incoming frame header claims.
// Writes into the caller's buffer must therefore be bounded by this constant,
// never by hdr->channels.
#define AUDIO_OUT_CHANNELS 2

// Early/late threshold: how far a frame may be early (held as pending) or late
// (dropped) before the timing engine acts.  Buffered AirPlay 2 streams have a
// deep jitter buffer so a tight threshold keeps sync without drop-outs.
// Unbuffered realtime streams (ALAC/UDP) have almost no buffer to absorb
// scheduling hiccups — e.g. when artwork/metadata arrives on the RTSP
// connection — so they need a much looser threshold to avoid audible
// drop-outs.  (In upstream these are Kconfig-driven; ESPHome has no Kconfig,
// so the defaults below apply.  They can be overridden via build flags.)
#ifdef CONFIG_AIRPLAY_TIMING_THRESHOLD_MS
#define TIMING_THRESHOLD_US (CONFIG_AIRPLAY_TIMING_THRESHOLD_MS * 1000)
#else
#define TIMING_THRESHOLD_US 25000  // 25ms early/late threshold (buffered)
#endif

#ifdef CONFIG_AIRPLAY_RT_TIMING_THRESHOLD_MS
#define RT_TIMING_THRESHOLD_US (CONFIG_AIRPLAY_RT_TIMING_THRESHOLD_MS * 1000)
#else
#define RT_TIMING_THRESHOLD_US 50000  // 50ms early/late threshold (realtime)
#endif
// MAX_CONSECUTIVE_EARLY: safety valve — counts how many consecutive calls to
// audio_timing_read returned silence because the pending frame was still too
// early.  Each call corresponds to one DMA period (~46 ms on I2S at 44100 Hz).
// 50 calls × 46 ms ≈ 2.3 s: long enough that a legitimate pre-buffer of any
// realistic depth will never hit it, short enough to detect a genuinely stuck
// or invalid anchor in a few seconds.
#define MAX_CONSECUTIVE_EARLY 50

namespace esphome {
namespace airplay_receiver {

namespace {

const char *const TAG = "audio_time";

enum sync_mode_t {
  SYNC_MODE_NONE,  // No clock sync, use local anchor time
  SYNC_MODE_PTP,   // AirPlay 2 PTP sync
  SYNC_MODE_NTP,   // AirPlay 1 NTP sync
};

uint32_t frame_samples_from_format(const audio_format_t *format) {
  if (format->frame_size > 0) {
    return static_cast<uint32_t>(format->frame_size);
  }
  if (format->max_samples_per_frame > 0) {
    return format->max_samples_per_frame;
  }
  return AAC_FRAMES_PER_PACKET;
}

void update_timing_targets(audio_timing_t *timing, const audio_format_t *format) {
  timing->nominal_frame_samples = frame_samples_from_format(format);

  if (format->sample_rate <= 0 || timing->nominal_frame_samples == 0) {
    timing->target_buffer_frames = MIN_STARTUP_FRAMES;
    return;
  }

  uint64_t latency_samples =
      ((uint64_t) timing->output_latency_us * (uint64_t) format->sample_rate) /
      1000000ULL;
  uint32_t target_frames =
      (uint32_t) ((latency_samples + timing->nominal_frame_samples - 1) /
                  timing->nominal_frame_samples);
  if (target_frames < MIN_STARTUP_FRAMES) {
    target_frames = MIN_STARTUP_FRAMES;
  }
  timing->target_buffer_frames = target_frames;
}

// Compute how early (positive) or late (negative) a frame is in microseconds.
bool compute_early_us(const audio_timing_t *timing, const audio_format_t *format,
                      uint32_t rtp_timestamp, sync_mode_t sync_mode, int64_t *early_us) {
  if (!timing->anchor_valid || format->sample_rate <= 0) {
    return false;
  }

  int32_t rtp_delta = static_cast<int32_t>(rtp_timestamp - timing->anchor_rtp_time);
  int64_t frame_offset_ns =
      ((int64_t) rtp_delta * 1000000000LL) / format->sample_rate;

  // Stream playout latency.  For AirPlay 2 REALTIME streams (type 96) the
  // anchor maps an RTP timestamp onto the sender's source timeline, and the
  // receiver is expected to emit that frame `latencyMin` samples LATER —
  // 11025 samples (250 ms) unless SETUP negotiates otherwise.  Every
  // reference receiver applies this delay; playing at the anchor instant
  // directly makes this device lead the whole group by exactly 250 ms.
  //
  // Buffered streams (type 103) schedule playout with the anchor directly
  // and keep this at 0.
  int64_t latency_ns =
      ((int64_t) timing->playout_latency_samples * 1000000000LL) /
      format->sample_rate;

  int64_t target_ns;
  switch (sync_mode) {
  case SYNC_MODE_PTP:
    // AirPlay 2: use network time with PTP offset for multi-room sync.
    target_ns = (int64_t) timing->anchor_network_time_ns -
                ptp_clock_get_offset_ns() + frame_offset_ns + latency_ns;
    break;
  case SYNC_MODE_NTP:
    // AirPlay 1: use network time with NTP offset for multi-room sync.
    // offset = remote_time - local_time, so local = remote - offset.
    target_ns = (int64_t) timing->anchor_network_time_ns -
                ntp_clock_get_offset_ns() + frame_offset_ns + latency_ns;
    break;
  default:
    // Fallback: use local anchor time (no multi-room sync).
    target_ns = timing->anchor_local_time_ns + frame_offset_ns + latency_ns;
    break;
  }

  // Subtract the output pipeline delay: the time between this call handing a
  // sample to the backend and that sample leaving the DAC.
  //
  // Prefer the LIVE measurement (frames accepted by the hardware minus frames
  // the hardware reports as clocked out).  The modelled constant is only a
  // fallback for backends without a completion cursor, and it is wrong in the
  // two situations that matter:
  //   - it assumes the ring is always at its steady-state occupancy, so when
  //     the playback task is briefly starved the read happens late against a
  //     ring that is emptier than modelled and the error reads far more
  //     negative than reality (the one-sided noise the position servo's
  //     innovation clamp was added to survive);
  //   - after a real underrun the ring is dry, so the next sample is heard
  //     almost immediately rather than ~43 ms later.  With the model the
  //     engine cannot tell the difference and the lost output time is never
  //     recovered.
  //
  // The live depth is self-consistent while the ring refills: each frame
  // consumed adds its own duration to the queue, so the measured error stays
  // put instead of walking, and no extra frames are dropped during recovery.
  int64_t now_us = 0;
  uint32_t pipeline_us = 0;
  if (!audio_output_get_pipeline_us(&now_us, &pipeline_us)) {
    now_us = esp_timer_get_time();
    pipeline_us = audio_output_get_hardware_latency_us();
  }
  target_ns -= (int64_t) (pipeline_us + PIPELINE_LATENCY_US) * 1000LL;

  *early_us = (target_ns / 1000LL) - now_us;

  return true;
}

// Index of the quietest sample in a frame (smallest summed magnitude across
// the output channels).  Servo trims drop or duplicate exactly one sample;
// doing that at the frame's quietest point makes the waveform seam
// inaudible even on loud tonal content, where trimming the frame's last
// sample regardless of amplitude could produce a faint click.
size_t quietest_sample_index(const int16_t *pcm, size_t frame_samples, size_t channels,
                             size_t out_ch) {
  size_t best = frame_samples - 1;
  int32_t best_mag = INT32_MAX;
  for (size_t i = 0; i < frame_samples; i++) {
    int32_t mag = 0;
    for (size_t ch = 0; ch < out_ch; ch++) {
      int32_t s = pcm[i * channels + ch];
      mag += s < 0 ? -s : s;
    }
    if (mag < best_mag) {
      best_mag = mag;
      best = i;
    }
  }
  return best;
}

}  // namespace

void audio_timing_init(audio_timing_t *timing, size_t pending_capacity) {
  if (!timing) {
    return;
  }

  memset(timing, 0, sizeof(*timing));
  timing->output_latency_us = DEFAULT_BUFFER_LATENCY_US;
  timing->playing = true;

  if (pending_capacity > 0) {
    timing->pending_frame = static_cast<uint8_t *>(airplay_alloc(pending_capacity, true));
    if (timing->pending_frame) {
      timing->pending_frame_capacity = pending_capacity;
    }
  }
}

void audio_timing_reset_continuity(audio_timing_t *timing) {
  if (!timing) {
    return;
  }
  timing->expected_rtp_valid = false;
  timing->pos_err_filtered_us = 0;
  timing->servo_engaged = false;
  timing->servo_phase = 0;
}

void audio_timing_reset(audio_timing_t *timing) {
  if (!timing) {
    return;
  }

  timing->playout_started = false;
  timing->anchor_valid = false;
  timing->pending_valid = false;
  timing->pending_frame_len = 0;
  timing->ready_time_us = 0;
  timing->consecutive_early_frames = 0;
  timing->quick_start = false;
  timing->deferred_flush_pending = false;
  timing->flush_until_ts = 0;
  timing->late_drop_count = 0;
  timing->late_drop_active = false;
  timing->servo_trims = 0;
  audio_timing_reset_continuity(timing);
}

void audio_timing_set_format(audio_timing_t *timing, const audio_format_t *format) {
  if (!timing || !format) {
    return;
  }

  update_timing_targets(timing, format);
}

void audio_timing_set_output_latency(audio_timing_t *timing, const audio_format_t *format,
                                     uint32_t latency_us) {
  if (!timing || !format) {
    return;
  }

  timing->output_latency_us = latency_us;
  update_timing_targets(timing, format);
}

uint32_t audio_timing_get_output_latency(const audio_timing_t *timing) {
  if (!timing) {
    return 0;
  }

  return timing->output_latency_us;
}

uint32_t audio_timing_get_hardware_latency(void) {
  return audio_output_get_hardware_latency_us();
}

uint32_t audio_timing_get_advertised_latency(const audio_timing_t *timing) {
  // Total end-to-end latency between the phone scheduling a frame and the
  // DAC emitting it.  See the header file's note: DIAGNOSTIC ONLY.
  uint32_t base = timing ? timing->output_latency_us : DEFAULT_BUFFER_LATENCY_US;
  return base + audio_output_get_hardware_latency_us() + PIPELINE_LATENCY_US;
}

void audio_timing_set_playout_latency(audio_timing_t *timing, uint32_t latency_samples) {
  if (!timing) {
    return;
  }
  timing->playout_latency_samples = latency_samples;
}

void audio_timing_set_anchor(audio_timing_t *timing, const audio_format_t *format,
                             uint64_t clock_id, uint64_t network_time_ns, uint32_t rtp_time) {
  if (!timing || !format) {
    return;
  }

  (void) clock_id;

  int64_t now_ns = (int64_t) esp_timer_get_time() * 1000LL;

  timing->anchor_rtp_time = rtp_time;
  timing->anchor_network_time_ns = network_time_ns;
  timing->anchor_local_time_ns = now_ns;
  timing->ptp_locked = ptp_clock_is_locked();
  timing->anchor_valid = true;
  // Reset frame counters so pre-buffered audio after a pause/resume or
  // track skip does not accumulate into the new anchor's counts.
  timing->consecutive_early_frames = 0;

  // Compute lead time: how far in the future this anchor's network timestamp
  // is relative to now.  Negative means the anchor is already in the past
  // (normal: the phone pre-buffers and the anchor is 200–800 ms old by the
  // time we receive it).
  int64_t lead_ms =
      ((int64_t) network_time_ns - (int64_t) (ptp_clock_get_offset_ns() + now_ns)) / 1000000LL;
  ESP_LOGI(TAG,
           "Anchor set: rtp=%" PRIu32 " lead=%lld ms ptp_locked=%d quick_start=%d", rtp_time,
           (long long) lead_ms, timing->ptp_locked, timing->quick_start);
}

void audio_timing_set_playing(audio_timing_t *timing, bool playing) {
  if (!timing) {
    return;
  }

  ESP_LOGI(TAG, "set_playing: %s -> %s", timing->playing ? "playing" : "paused",
           playing ? "playing" : "paused");

  timing->playing = playing;
  if (!playing) {
    // Discard any partially-pending frame so resume starts cleanly from
    // the oldest frame in the sorted buffer.
    timing->pending_valid = false;
    timing->pending_frame_len = 0;
  }
}

size_t audio_timing_read(audio_timing_t *timing, audio_buffer_t *buffer,
                         const audio_stream_t *stream, audio_stats_t *stats, int16_t *out,
                         size_t samples) {
  if (!timing || !buffer || !stream || !out || samples == 0) {
    return 0;
  }

  if (!timing->playing) {
    return 0;
  }

  const audio_format_t *format = &stream->format;
  int buffered_frames = audio_buffer_get_frame_count(buffer);

  // Unbuffered realtime streams (ALAC/UDP) get a looser early/late threshold
  // than buffered AirPlay 2 streams, because they have little jitter buffer to
  // absorb scheduling hiccups and would otherwise drop frames (audible
  // drop-outs) whenever the pipeline stalls — e.g. while artwork/metadata is
  // received on the RTSP connection.
  const int64_t timing_threshold_us =
      audio_stream_uses_buffer(stream->type) ? TIMING_THRESHOLD_US : RT_TIMING_THRESHOLD_US;

  // Wait for enough buffer before starting.
  // In quick_start mode (after a seek/skip), start as soon as 1 frame is
  // available to minimise the gap between tracks.  Anchor-based timing
  // still applies — if the frame is early, silence is output until its
  // scheduled play time, just like shairport-sync.
  // Normal startup waits for target_buffer_frames to build jitter margin.
  if (!timing->playout_started && !timing->pending_valid) {
    int required = timing->quick_start ? 1 : (int) timing->target_buffer_frames;
    if (buffered_frames < required) {
      return 0;
    }
    // Wait for anchor before playing.
    // Normal startup: allow a 1-second fallback so a stream with no anchor
    // (e.g. AirPlay 1 without NTP) can still start.
    if (!timing->anchor_valid) {
      int64_t now_us = esp_timer_get_time();
      if (timing->ready_time_us == 0) {
        timing->ready_time_us = now_us;
      }
      if (now_us - timing->ready_time_us < 1000000) {
        return 0;  // Still waiting for anchor
      }
      // Waited 1 second, no anchor - proceed without sync.
    }
  }

  // Determine sync mode: PTP (AirPlay 2), NTP (AirPlay 1), or local fallback.
  sync_mode_t sync_mode = SYNC_MODE_NONE;
  if (ptp_clock_is_locked()) {
    sync_mode = SYNC_MODE_PTP;
  } else if (ntp_clock_is_locked()) {
    sync_mode = SYNC_MODE_NTP;
  }

  // Keep the playout task bounded.  Stale frames are still drained in batches,
  // but a single call may spend at most ~1.5 ms here.
  enum { MAX_DRAIN_ATTEMPTS = 64 };
  const int64_t drain_deadline_us = esp_timer_get_time() + 1500;
  // Set when the drain loop below discards at least one late frame in this
  // call, or when a previous call left a drop run unfinished.  A drop run
  // re-locks the stream onto a new position, so both the release point for
  // the next playable frame and the point at which dropping stops tighten
  // from the wide jitter threshold to half a frame period.
  bool dropped_late = timing->late_drop_active;
  // Stale start-island frames skipped in this call.
  int start_skips = 0;
  for (int attempt = 0; attempt < MAX_DRAIN_ATTEMPTS; attempt++) {
    // Check the deadline every eight iterations to limit esp_timer overhead.
    if ((attempt & 7) == 0 && attempt != 0 && esp_timer_get_time() >= drain_deadline_us) {
      break;
    }
    size_t item_size = 0;
    void *item = NULL;
    bool from_pending = false;

    // Get frame from pending or buffer.
    if (timing->pending_valid) {
      item_size = timing->pending_frame_len;
      if (item_size < sizeof(audio_frame_header_t)) {
        timing->pending_valid = false;
        timing->pending_frame_len = 0;
        continue;
      }
      item = timing->pending_frame;
      from_pending = true;
    } else {
      if (!audio_buffer_take(buffer, &item, &item_size, 0)) {
        if (stats) {
          stats->buffer_underruns++;
        }
        return 0;
      }
      buffered_frames = audio_buffer_get_frame_count(buffer);

      if (item_size < sizeof(audio_frame_header_t)) {
        audio_buffer_return(buffer, item);
        continue;
      }
    }

    audio_frame_header_t *hdr = (audio_frame_header_t *) item;
    size_t frame_samples = hdr->samples_per_channel;
    size_t channels = hdr->channels ? hdr->channels : format->channels;
    int16_t *pcm = (int16_t *) (hdr + 1);

    // Validate frame.
    if (frame_samples == 0 || channels == 0) {
      if (from_pending) {
        timing->pending_valid = false;
        timing->pending_frame_len = 0;
      } else {
        audio_buffer_return(buffer, item);
      }
      continue;
    }

    size_t expected_bytes = sizeof(*hdr) + frame_samples * channels * sizeof(int16_t);
    if (item_size < expected_bytes) {
      if (from_pending) {
        timing->pending_valid = false;
        timing->pending_frame_len = 0;
      } else {
        audio_buffer_return(buffer, item);
      }
      continue;
    }

    if (frame_samples > samples) {
      frame_samples = samples;
    }

    // Deferred flush check (AirPlay 2 FLUSHBUFFERED with flushFromSeq):
    // keep playing until the frame whose RTP timestamp reaches flush_until_ts,
    // then bulk-flush the remainder of the buffer and start fresh.
    // Signed 32-bit subtraction handles RTP wraparound correctly.
    if (timing->deferred_flush_pending) {
      if ((int32_t) (hdr->rtp_timestamp - timing->flush_until_ts) >= 0) {
        ESP_LOGI(TAG, "Deferred flush triggered at ts=%" PRIu32 " (until_ts=%" PRIu32 ")",
                 hdr->rtp_timestamp, timing->flush_until_ts);
        if (from_pending) {
          timing->pending_valid = false;
          timing->pending_frame_len = 0;
        } else {
          audio_buffer_return(buffer, item);
        }
        audio_buffer_flush(buffer);
        timing->deferred_flush_pending = false;
        audio_timing_reset_continuity(timing);
        timing->playout_started = false;
        timing->ready_time_us = 0;
        timing->consecutive_early_frames = 0;
        // quick_start so the first frame of the next track starts playing
        // as soon as 1 frame arrives, with normal anchor timing applied.
        timing->quick_start = true;
        return 0;
      }
    }

    // Handle early/late frames based on anchor timing.
    //
    // After a seek/flush, anchor-based timing is applied immediately from the
    // first frame — no bypass.  With a stable PTP clock the anchor is
    // accurate, so early frames are held as pending (silence output) until
    // their scheduled play time, and late frames are dropped.  This mirrors
    // shairport-sync's approach and guarantees the first audible sample is
    // correctly synchronised.
    // Stale start-island rejection.  A stream start (fresh or post-flush)
    // can leave a small ISLAND of stale frames stranded at the head of the
    // buffer, separated from the real stream by a large hole — typically
    // late retransmissions answering NACKs from before the flush, which
    // arrive after the RTP gates re-arm and fall inside their 10 s window.
    // Starting playout from such an island plays a ~100 ms blip of audio,
    // then the hole (concealed as silence), then the track — an audible pop
    // at every affected stream start.  Until playout has started, skip any
    // frame that sits more than ~100 ms below the start of the contiguous
    // run that ends at the newest received frame.
    if (!timing->playout_started && format->sample_rate > 0) {
      uint32_t bulk_rtp = 0;
      if (audio_buffer_bulk_start_rtp(buffer, &bulk_rtp)) {
        int32_t behind = (int32_t) (bulk_rtp - hdr->rtp_timestamp);
        if (behind > (int32_t) (format->sample_rate / 10)) {
          start_skips++;
          if (from_pending) {
            timing->pending_valid = false;
            timing->pending_frame_len = 0;
          } else {
            audio_buffer_return(buffer, item);
          }
          continue;
        }
      }
      if (start_skips > 0) {
        ESP_LOGI(TAG,
                 "Skipped %d stale start frame(s); contiguous stream begins at rtp=%" PRIu32,
                 start_skips, hdr->rtp_timestamp);
        start_skips = 0;
      }
    }

    // RTP continuity vs the frame just played.  A fresh frame ABOVE
    // expected_rtp means packets were lost and not recovered by the resend
    // mechanism: conceal the hole by holding the frame to the strict
    // release, so exactly gap-length silence plays at the right schedule.
    // Without this, the post-gap frame (typically ~8 ms early, far inside
    // the 50 ms realtime threshold) played immediately — an audible skip
    // AND a permanent early shift of the whole playback position for every
    // unrecovered packet.  At or below expected_rtp is contiguous audio (or
    // a duplicate from a redundant resend, which is played because repeating
    // at most one frame keeps the stream moving); its schedule slot begins
    // the instant the previous frame ends, so it is normally released
    // straight away by the wide threshold below.
    bool gap = false;
    if (!from_pending && timing->playout_started && timing->expected_rtp_valid) {
      int32_t cont_delta = (int32_t) (hdr->rtp_timestamp - timing->expected_rtp);
      if (cont_delta > 0) {
        gap = true;
        timing->gaps++;
        // Rate-limited: a burst of separate holes must not throttle this
        // path with blocking log writes.
        int64_t now_us = esp_timer_get_time();
        if (now_us - timing->last_gap_log_us > 250000) {
          ESP_LOGW(TAG,
                   "Gap: %ld samples (%ld ms) missing before rtp=%" PRIu32
                   " — concealing with silence (gaps=%" PRIu32 ", +%" PRIu32 " unlogged)",
                   (long) cont_delta,
                   (long) (cont_delta * 1000L / format->sample_rate),
                   hdr->rtp_timestamp, timing->gaps, timing->gaps_suppressed);
          timing->last_gap_log_us = now_us;
          timing->gaps_suppressed = 0;
        } else {
          timing->gaps_suppressed++;
        }
      }
    }

    if (timing->anchor_valid && format->sample_rate > 0) {
      int64_t early_us = 0;
      if (compute_early_us(timing, format, hdr->rtp_timestamp, sync_mode, &early_us)) {
        // Hold-release point.  A FRESH frame is shelved as pending when it
        // is more than the (wide, jitter-hysteresis) threshold early.  But a
        // frame ALREADY pending is re-checked once per frame period (~8 ms),
        // and must be held until its play time has actually arrived —
        // releasing it at the threshold instead starts playback up to
        // threshold_us early, and nothing downstream ever corrects position,
        // so the whole session inherits that bias.  Releasing at half a frame
        // period centres the startup error at 0 (±4 ms at 44.1 kHz).
        // Frames following a drop run or a detected RTP gap also use the
        // strict release, so discontinuity recovery re-locks position
        // precisely instead of anywhere inside the wide threshold.
        int64_t frame_period_us = ((int64_t) frame_samples * 1000000LL) / format->sample_rate;
        int64_t strict_us = frame_period_us / 2;
        int64_t release_us =
            (from_pending || dropped_late || gap) ? strict_us : timing_threshold_us;

        // Late gate, with hysteresis.  Lateness beyond the wide threshold
        // STARTS a drop run; once started, frames keep being dropped until
        // the stream is back on schedule rather than until it is merely
        // inside the threshold again.
        int64_t late_limit_us = dropped_late ? -strict_us : -timing_threshold_us;

        // A contiguous frame is no longer exempt from the early gate.
        if (early_us > release_us) {
          // Only advance the stuck-anchor counter for NEW frames taken from
          // the buffer — not for pending re-checks of the same early frame.
          if (!from_pending) {
            timing->consecutive_early_frames++;
            // Log the first early frame after each anchor set (shows lead
            // time before audio starts) and every 50 new frames after that.
            if (timing->consecutive_early_frames == 1) {
              ESP_LOGI(TAG,
                       "First early frame: rtp=%" PRIu32 " early=%.1f ms quick_start=%d "
                       "buffered=%d",
                       hdr->rtp_timestamp, (float) early_us / 1000.0f, timing->quick_start,
                       buffered_frames);
            } else if (timing->consecutive_early_frames % 50 == 0) {
              ESP_LOGD(TAG, "Early counter: %d/%d early=%.1f ms rtp=%" PRIu32,
                       timing->consecutive_early_frames, MAX_CONSECUTIVE_EARLY,
                       (float) early_us / 1000.0f, hdr->rtp_timestamp);
            }
          }

          // If we have had an implausibly long run of early frames the anchor
          // is probably stuck or wrong — give up on it so playback can
          // continue.  This threshold is high enough (~17 s at 23 ms/frame)
          // that it never fires during normal pre-buffered-audio scenarios.
          if (timing->consecutive_early_frames > MAX_CONSECUTIVE_EARLY) {
            ESP_LOGW(TAG,
                     "Invalidating stuck anchor: consecutive=%d, early=%lld ms",
                     timing->consecutive_early_frames, early_us / 1000LL);
            timing->anchor_valid = false;
            timing->consecutive_early_frames = 0;
            // Fall through to play the frame normally.
          } else {
            // Frame is early — store it as pending and output silence.
            static int early_count = 0;
            early_count++;
            if (early_count % 100 == 1) {
              ESP_LOGD(TAG,
                       "Frame too early #%d: %lld ms, buffered=%d, pending=%d", early_count,
                       early_us / 1000LL, buffered_frames, timing->pending_valid ? 1 : 0);
            }
            if (!from_pending && timing->pending_frame &&
                item_size <= timing->pending_frame_capacity) {
              memcpy(timing->pending_frame, item, item_size);
              timing->pending_frame_len = item_size;
              timing->pending_valid = true;
              audio_buffer_return(buffer, item);
            }
            // Emit one frame's worth of silence, not the caller's full
            // capacity.  Callers allocate exactly `samples` stereo frames
            // (see audio_output.cpp), so the write must be bounded by
            // AUDIO_OUT_CHANNELS rather than the frame header's channel
            // count — a malformed header claiming >2 channels would
            // otherwise overrun the caller's buffer.  Returning
            // frame_samples (not `samples`) also keeps the silence path's
            // output length consistent with the normal playback path, so
            // holding a frame pending does not advance the output clock
            // faster than playing one.
            memset(out, 0, frame_samples * AUDIO_OUT_CHANNELS * sizeof(int16_t));
            return frame_samples;
          }
        } else if (early_us < late_limit_us) {
          // Reset consecutive early counter on late/normal frames.
          timing->consecutive_early_frames = 0;

          // Late frame: count it and continue within the bounded drain budget.
          dropped_late = true;
          timing->late_drop_count++;
          timing->late_drop_active = true;
          // A dropped frame is consumed from the RTP sequence just like a
          // played one: advance the continuity marker past it (forward
          // only).  Without this, expected_rtp froze at the last PLAYED
          // frame during a drain, so every subsequent contiguous stale
          // frame was miscounted as a fresh loss.
          uint32_t drop_next = hdr->rtp_timestamp + hdr->samples_per_channel;
          if (!timing->expected_rtp_valid ||
              (int32_t) (drop_next - timing->expected_rtp) > 0) {
            timing->expected_rtp = drop_next;
            timing->expected_rtp_valid = true;
          }
          if (stats) {
            stats->late_frames++;
          }
          if (from_pending) {
            timing->pending_valid = false;
            timing->pending_frame_len = 0;
          } else {
            audio_buffer_return(buffer, item);
          }
          continue;
        }
      }
    }

    // Frame is on time (or anchor-invalid) — reset counter.
    timing->consecutive_early_frames = 0;

    // Snapshot metadata before the pool slot is returned below — reading
    // hdr fields after audio_buffer_return would be a use-after-return.
    uint32_t played_rtp_timestamp = hdr->rtp_timestamp;
    uint32_t played_samples = hdr->samples_per_channel;

    // Playout report (diagnostic) and position servo.
    int sample_adjust = 0;
    if (timing->anchor_valid && sync_mode != SYNC_MODE_NONE && timing->playout_started) {
      int64_t on_time_err_us = 0;
      if (compute_early_us(timing, format, played_rtp_timestamp, sync_mode, &on_time_err_us)) {
        if (timing->playout_reports++ % 125 == 0) {
          uint32_t newest_rtp = 0;
          int64_t depth_ms = -1;
          if (audio_buffer_peek_newest_rtp(buffer, &newest_rtp)) {
            depth_ms = ((int64_t) (int32_t) (newest_rtp - played_rtp_timestamp) * 1000LL) /
                       format->sample_rate;
          }
          // PTP filter divergence.  err/lead/depth are all computed THROUGH
          // filtered_offset_ns, so none of them can reveal an error in that
          // offset — the device converges perfectly onto a displaced target
          // and reports err=0 the whole time.  Comparing the filtered offset
          // against the most recent raw sample is the only independent check
          // available on-device.
          ptp_stats_t ps;
          ptp_clock_get_stats(&ps);
          int64_t ptp_gap_us = (ps.last_offset_ns - ps.filtered_offset_ns) / 1000LL;
          // under: output underruns since boot.  Any increase means the
          // playback task was starved long enough for the DMA ring to run
          // dry, which is the disturbance the drain has to clean up after.
          ESP_LOGI(TAG,
                   "Playout: err=%lld ms buffered=%d depth=%lld ms ptp_gap=%lld us "
                   "outliers=%" PRIu32 " gaps=%" PRIu32 " under=%" PRIu32 " rtp=%" PRIu32,
                   (long long) (on_time_err_us / 1000LL), buffered_frames,
                   (long long) depth_ms, (long long) ptp_gap_us, ps.outlier_count, timing->gaps,
                   audio_output_get_underruns(), played_rtp_timestamp);
        }

        // Position servo (see POS_SERVO_* above).  Smooth the per-frame
        // error with a clamped-innovation IIR (robust to one-sided
        // late-read measurement spikes), engage outside the hysteresis
        // band, trim one sample per POS_SERVO_TRIM_INTERVAL frames until
        // the error is back inside.
        int64_t innovation = on_time_err_us - timing->pos_err_filtered_us;
        if (innovation > POS_SERVO_INNOV_CLAMP_US) {
          innovation = POS_SERVO_INNOV_CLAMP_US;
        } else if (innovation < -POS_SERVO_INNOV_CLAMP_US) {
          innovation = -POS_SERVO_INNOV_CLAMP_US;
        }
        timing->pos_err_filtered_us += innovation / POS_SERVO_FILTER_DIV;

        int64_t abs_err = timing->pos_err_filtered_us < 0 ? -timing->pos_err_filtered_us
                                                          : timing->pos_err_filtered_us;
        if (!timing->servo_engaged && abs_err > POS_SERVO_ENGAGE_US) {
          timing->servo_engaged = true;
          timing->servo_phase = 0;
          ESP_LOGI(TAG, "Position servo engaged: err=%lld us",
                   (long long) timing->pos_err_filtered_us);
        } else if (timing->servo_engaged && abs_err < POS_SERVO_DISENGAGE_US) {
          timing->servo_engaged = false;
          ESP_LOGI(TAG, "Position servo disengaged: err=%lld us trims=%" PRIu32,
                   (long long) timing->pos_err_filtered_us, timing->servo_trims);
        }

        if (timing->servo_engaged && ++timing->servo_phase >= POS_SERVO_TRIM_INTERVAL) {
          timing->servo_phase = 0;
          // Positive error = playing early = stretch (emit one extra sample)
          // so playout slows; negative = late = shrink to catch up.
          sample_adjust = timing->pos_err_filtered_us > 0 ? 1 : -1;
          timing->servo_trims++;
        }
      }
    }

    // Apply the servo's one-sample trim.  Shrink: copy one sample fewer.
    // Stretch: copy the frame then repeat its final sample once.  The
    // stretch stays within the caller's capacity (`samples` frames, which
    // is FRAME_SAMPLES + 1 in every output backend).
    size_t out_samples = frame_samples;
    if (sample_adjust < 0 && out_samples > 1) {
      out_samples--;
    } else if (sample_adjust > 0 && out_samples + 1 <= samples) {
      out_samples++;
    }

    // Copy PCM data to output.  The output buffer is interleaved stereo of
    // exactly `samples` frames, so never write more than AUDIO_OUT_CHANNELS
    // per sample regardless of what the frame header claims.  `channels` is
    // still used to step through the SOURCE frame, which was
    // length-validated above against expected_bytes.
    size_t out_ch = channels < AUDIO_OUT_CHANNELS ? channels : AUDIO_OUT_CHANNELS;
    if (out_samples == frame_samples) {
      if (out_ch == channels) {
        memcpy(out, pcm, frame_samples * out_ch * sizeof(int16_t));
      } else {
        // Source has more channels than we emit — take the leading out_ch.
        for (size_t i = 0; i < frame_samples; i++) {
          for (size_t ch = 0; ch < out_ch; ch++) {
            out[i * out_ch + ch] = pcm[i * channels + ch];
          }
        }
      }
    } else {
      // Servo trim: drop or duplicate exactly one sample, placed at the
      // frame's quietest point so the seam is inaudible.
      size_t m = quietest_sample_index(pcm, frame_samples, channels, out_ch);
      size_t o = 0;
      for (size_t i = 0; i < frame_samples; i++) {
        if (out_samples < frame_samples && i == m) {
          continue;  // shrink: skip the quietest sample
        }
        for (size_t ch = 0; ch < out_ch; ch++) {
          out[o * out_ch + ch] = pcm[i * channels + ch];
        }
        o++;
        if (out_samples > frame_samples && i == m) {
          // stretch: duplicate the quietest sample
          for (size_t ch = 0; ch < out_ch; ch++) {
            out[o * out_ch + ch] = pcm[i * channels + ch];
          }
          o++;
        }
      }
    }

    // The next contiguous frame starts where this one's SOURCE data ends
    // (servo trims alter output length, not source consumption).  Forward
    // only: playing a duplicate (redundant resend) must not rewind the
    // marker, which would misread the following real frame as a gap.
    uint32_t play_next = played_rtp_timestamp + played_samples;
    if (!timing->expected_rtp_valid || (int32_t) (play_next - timing->expected_rtp) > 0) {
      timing->expected_rtp = play_next;
      timing->expected_rtp_valid = true;
    }

    // Cleanup.
    if (from_pending) {
      timing->pending_valid = false;
      timing->pending_frame_len = 0;
    } else {
      audio_buffer_return(buffer, item);
    }

    if (!timing->playout_started) {
      timing->playout_started = true;
      bool was_quick = timing->quick_start;
      timing->quick_start = false;
      ESP_LOGI(TAG, "Playout started%s: rtp=%" PRIu32, was_quick ? " (quick_start)" : "",
               played_rtp_timestamp);
    }

    if (timing->late_drop_active) {
      ESP_LOGW(TAG, "Late-frame drain complete: dropped=%" PRIu32, timing->late_drop_count);
      timing->late_drop_count = 0;
      timing->late_drop_active = false;
    }

    return out_samples;
  }

  return 0;
}

}  // namespace airplay_receiver
}  // namespace esphome
