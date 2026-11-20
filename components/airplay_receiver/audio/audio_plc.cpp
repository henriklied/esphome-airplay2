#include "audio_plc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../allocator.h"

namespace esphome {
namespace airplay_receiver {

namespace {

constexpr float PI = 3.14159265358979f;
/* Coarse-search energy per decimated value below which a signal counts as
 * silent: an RMS of ~64 per channel. */
constexpr float SILENT_ENERGY_PER_POINT =
    64.0f * 64.0f * PLC_DECIMATION * PLC_DECIMATION;
/* Any period loops silence seamlessly. */
constexpr uint32_t SILENT_PERIOD = PLC_MIN_PERIOD;


/* Frame `i` counted from the matched edge. */
inline uint32_t edge_frame(uint32_t frames, uint32_t i, bool at_end) {
  return at_end ? frames - 1U - i : i;
}

/* 1 -> 0 raised cosine over x in [0, 1]. */
inline float fade_out(float x) { return 0.5f * (1.0f + std::cos(PI * x)); }

inline int16_t clamp16(float v) {
  if (v > 32767.0f) {
    return 32767;
  }
  if (v < -32768.0f) {
    return -32768;
  }
  return static_cast<int16_t>(std::lround(v));
}

float full_rate_correlation(const int16_t *pcm, uint32_t frames, bool at_end,
                            uint32_t lag) {
  float dot = 0.0f, reference = 0.0f, candidate = 0.0f;
  for (uint32_t i = 0; i < PLC_FINE_FRAMES; i++) {
    const int16_t *a = &pcm[edge_frame(frames, i, at_end) * PLC_STRIDE];
    const int16_t *b = &pcm[edge_frame(frames, i + lag, at_end) * PLC_STRIDE];
    for (uint32_t ch = 0; ch < PLC_STRIDE; ch++) {
      dot += float(a[ch]) * float(b[ch]);
      reference += float(a[ch]) * float(a[ch]);
      candidate += float(b[ch]) * float(b[ch]);
    }
  }
  return reference > 0.0f && candidate > 0.0f
             ? dot / std::sqrt(reference * candidate)
             : 0.0f;
}

void free_buffers(audio_plc_t *plc) {
  airplay_free(plc->history);
  airplay_free(plc->past);
  airplay_free(plc->future);
  airplay_free(plc->analysis);
  plc->history = plc->past = plc->future = nullptr;
  plc->analysis = nullptr;
}

inline bool enabled(const audio_plc_t *plc) { return plc->history != nullptr; }

void push_history(audio_plc_t *plc, const int16_t *frame) {
  if (plc->history) {
    memcpy(&plc->history[plc->history_head * PLC_STRIDE], frame,
           PLC_STRIDE * sizeof(int16_t));
    plc->history_head = (plc->history_head + 1U) % PLC_HISTORY_FRAMES;
    if (plc->history_count < PLC_HISTORY_FRAMES) {
      plc->history_count++;
    }
  }
  plc->last[0] = frame[0];
  plc->last[1] = frame[1];
}

/* Forward loop: the last `period` frames of the past, with its end blended
 * toward the frames one period earlier so the wrap back to its start is the
 * signal's own continuation. */
float forward_sample(const audio_plc_t *plc, uint32_t j, uint32_t ch) {
  const uint32_t frames = plc->past_frames;
  const uint32_t period = plc->forward_period;
  if (period == 0U) {
    return plc->forward_hold[ch];
  }
  const uint32_t k = j % period;
  const float base = plc->past[(frames - period + k) * PLC_STRIDE + ch];
  if (k + PLC_SEAM_FRAMES < period || frames < 2U * period) {
    return base;
  }
  const float w = float(k + PLC_SEAM_FRAMES - period + 1U) /
                  float(PLC_SEAM_FRAMES + 1U);
  const float earlier = plc->past[(frames - 2U * period + k) * PLC_STRIDE + ch];
  return (1.0f - w) * base + w * earlier;
}

/* Backward loop: the first `period` frames of the future, with its start
 * blended from the frames one period later so the loop's own wrap is smooth.
 * Index 0 is the frame the loop places just after its last frame. */
float backward_loop(const audio_plc_t *plc, uint32_t index, uint32_t ch) {
  const uint32_t period = plc->backward_period;
  const float base = plc->future[index * PLC_STRIDE + ch];
  if (index >= PLC_SEAM_FRAMES) {
    return base;
  }
  const float w = float(index + 1U) / float(PLC_SEAM_FRAMES + 1U);
  return (1.0f - w) * plc->future[(period + index) * PLC_STRIDE + ch] + w * base;
}

/* Frame `j` of the planned hole. */
void synth_frame(const audio_plc_t *plc, uint32_t j, int16_t *frame) {
  frame[0] = frame[1] = 0;
  if (!enabled(plc) || j >= plc->fill_frames) {
    return;
  }
  const uint32_t hole = plc->fill_frames;
  float forward_gain = 0.0f;
  float backward_gain = 0.0f;
  if (!plc->end_known) {
    if (j < PLC_HOLD_FRAMES) {
      forward_gain = 1.0f;
    } else if (j < PLC_HOLD_FRAMES + PLC_FADE_FRAMES) {
      forward_gain =
          fade_out((float(j - PLC_HOLD_FRAMES) + 0.5f) / PLC_FADE_FRAMES);
    }
  } else if (hole <= 2U * PLC_CROSSFADE_FRAMES) {
    forward_gain = fade_out((float(j) + 0.5f) / float(hole));
    backward_gain = 1.0f - forward_gain;
  } else {
    if (j < PLC_CROSSFADE_FRAMES) {
      forward_gain = fade_out((float(j) + 0.5f) / PLC_CROSSFADE_FRAMES);
    }
    if (j >= hole - PLC_CROSSFADE_FRAMES) {
      backward_gain =
          1.0f - fade_out((float(j - (hole - PLC_CROSSFADE_FRAMES)) + 0.5f) /
                          PLC_CROSSFADE_FRAMES);
    }
  }

  uint32_t backward_index = 0U;
  if (backward_gain > 0.0f && plc->backward_period > 0U) {
    const uint32_t period = plc->backward_period;
    backward_index = (period - (hole - j) % period) % period;
  }
  for (uint32_t ch = 0; ch < PLC_STRIDE; ch++) {
    float value = 0.0f;
    if (forward_gain > 0.0f) {
      value += forward_gain * forward_sample(plc, j, ch);
    }
    if (backward_gain > 0.0f) {
      const float backward = plc->backward_period > 0U
                                 ? backward_loop(plc, backward_index, ch)
                                 : float(plc->future[ch]);
      value += backward_gain * backward;
    }
    frame[ch] = clamp16(value);
  }
}

}  // namespace

uint32_t audio_plc_find_period(const int16_t *pcm, uint32_t frames, bool at_end,
                               float *scratch) {
  if (!pcm || !scratch || frames < PLC_MIN_PERIOD + PLC_MATCH_FRAMES) {
    return 0U;
  }
  const uint32_t max_lag = std::min(PLC_MAX_PERIOD, frames - PLC_MATCH_FRAMES);
  const uint32_t points = (max_lag + PLC_MATCH_FRAMES) / PLC_DECIMATION;
  /* Channels stay separate: a mono mix of anti-phase content is silence. */
  for (uint32_t p = 0; p < points; p++) {
    for (uint32_t ch = 0; ch < PLC_STRIDE; ch++) {
      int32_t sum = 0;
      for (uint32_t d = 0; d < PLC_DECIMATION; d++) {
        sum += pcm[edge_frame(frames, p * PLC_DECIMATION + d, at_end) *
                       PLC_STRIDE + ch];
      }
      scratch[p * PLC_STRIDE + ch] = static_cast<float>(sum);
    }
  }

  const uint32_t window = PLC_MATCH_FRAMES / PLC_DECIMATION;
  const uint32_t window_values = window * PLC_STRIDE;
  float reference = 0.0f;
  for (uint32_t i = 0; i < window_values; i++) {
    reference += scratch[i] * scratch[i];
  }
  if (reference < SILENT_ENERGY_PER_POINT * window_values) {
    return SILENT_PERIOD;
  }

  uint32_t best = PLC_MIN_PERIOD / PLC_DECIMATION;
  float best_score = -2.0f;
  for (uint32_t lag = PLC_MIN_PERIOD / PLC_DECIMATION;
       lag * PLC_DECIMATION <= max_lag && lag + window <= points; lag++) {
    float dot = 0.0f, candidate = 0.0f;
    const float *shifted = &scratch[lag * PLC_STRIDE];
    for (uint32_t i = 0; i < window_values; i++) {
      dot += scratch[i] * shifted[i];
      candidate += shifted[i] * shifted[i];
    }
    const float score =
        candidate > 0.0f ? dot / std::sqrt(reference * candidate) : -1.0f;
    if (score > best_score) {
      best_score = score;
      best = lag;
    }
  }

  const uint32_t centre = best * PLC_DECIMATION;
  const uint32_t low = std::max(PLC_MIN_PERIOD, centre - PLC_DECIMATION);
  const uint32_t high = std::min(max_lag, centre + PLC_DECIMATION);
  uint32_t period = std::min(std::max(centre, PLC_MIN_PERIOD), max_lag);
  best_score = -2.0f;
  for (uint32_t lag = low; lag <= high; lag++) {
    const float score = full_rate_correlation(pcm, frames, at_end, lag);
    if (score > best_score) {
      best_score = score;
      period = lag;
    }
  }
  return period;
}

bool audio_plc_init(audio_plc_t *plc) {
  memset(plc, 0, sizeof(*plc));
  const size_t frame_bytes = PLC_STRIDE * sizeof(int16_t);
  plc->history = static_cast<int16_t *>(
      airplay_alloc(PLC_HISTORY_FRAMES * frame_bytes, /*realtime=*/false));
  plc->past = static_cast<int16_t *>(
      airplay_alloc(PLC_HISTORY_FRAMES * frame_bytes, /*realtime=*/false));
  plc->future = static_cast<int16_t *>(
      airplay_alloc(PLC_FUTURE_FRAMES * frame_bytes, /*realtime=*/false));
  /* Internal RAM: the coarse search walks it ~100 times per hole. */
  plc->analysis = static_cast<float *>(
      airplay_alloc(PLC_PAST_FRAMES / PLC_DECIMATION * PLC_STRIDE *
                        sizeof(float),
                    /*realtime=*/true));
  const bool ok = plc->history && plc->past && plc->future && plc->analysis;
  if (!ok) {
    free_buffers(plc);
  }
  audio_plc_reset(plc);
  return ok;
}

void audio_plc_deinit(audio_plc_t *plc) {
  free_buffers(plc);
  audio_plc_reset(plc);
}

void audio_plc_reset(audio_plc_t *plc) {
  plc->history_head = 0U;
  plc->history_count = 0U;
  plc->past_frames = 0U;
  plc->future_frames = 0U;
  plc->forward_period = 0U;
  plc->backward_period = 0U;
  plc->fill_frames = 0U;
  plc->fill_pos = 0U;
  plc->blend_pos = PLC_SEAM_FRAMES;
  plc->blend_from_tail = false;
  plc->active = false;
  plc->end_known = false;
  plc->last[0] = plc->last[1] = 0;
  plc->blend_hold[0] = plc->blend_hold[1] = 0;
  plc->forward_hold[0] = plc->forward_hold[1] = 0;
}

void audio_plc_begin(audio_plc_t *plc, uint32_t hole_frames,
                     uint32_t future_frames) {
  plc->active = true;
  plc->blend_pos = PLC_SEAM_FRAMES;
  plc->fill_pos = 0U;
  plc->fill_frames = std::min(hole_frames, PLC_MAX_FILL_FRAMES);
  plc->end_known = future_frames > 0U && hole_frames <= PLC_MAX_FILL_FRAMES;
  plc->future_frames = plc->end_known ? future_frames : 0U;
  plc->forward_hold[0] = plc->last[0];
  plc->forward_hold[1] = plc->last[1];
  if (plc->end_known) {
    plc->two_sided_events++;
  } else {
    plc->forward_events++;
  }
  if (!enabled(plc)) {
    return;
  }

  const uint32_t frames = std::min(plc->history_count, PLC_PAST_FRAMES);
  const uint32_t oldest =
      (plc->history_head + PLC_HISTORY_FRAMES - frames) % PLC_HISTORY_FRAMES;
  for (uint32_t i = 0; i < frames; i++) {
    const uint32_t src = (oldest + i) % PLC_HISTORY_FRAMES;
    plc->past[i * PLC_STRIDE] = plc->history[src * PLC_STRIDE];
    plc->past[i * PLC_STRIDE + 1] = plc->history[src * PLC_STRIDE + 1];
  }
  plc->past_frames = frames;
  plc->forward_period =
      audio_plc_find_period(plc->past, frames, /*at_end=*/true, plc->analysis);
  plc->backward_period =
      plc->end_known ? audio_plc_find_period(plc->future, plc->future_frames,
                                             /*at_end=*/false, plc->analysis)
                     : 0U;
}

void audio_plc_conceal(audio_plc_t *plc, int16_t *out, size_t frames,
                       uint8_t channels) {
  for (size_t i = 0; i < frames; i++) {
    int16_t frame[PLC_STRIDE] = {0, 0};
    if (plc->active) {
      synth_frame(plc, plc->fill_pos, frame);
    }
    plc->fill_pos++;
    for (uint8_t ch = 0; ch < channels; ch++) {
      out[i * channels + ch] = frame[ch];
    }
    if (channels == 1U) {
      frame[1] = frame[0];
    }
    push_history(plc, frame);
  }
}

void audio_plc_recover(audio_plc_t *plc, int16_t *pcm, size_t frames,
                       uint8_t channels) {
  for (size_t i = 0; i < frames; i++) {
    int16_t *in = &pcm[i * channels];
    int16_t frame[PLC_STRIDE] = {in[0], channels > 1U ? in[1] : in[0]};
    if (plc->active) {
      plc->active = false;
      plc->blend_pos = 0U;
      plc->blend_from_tail = enabled(plc) && plc->end_known &&
                             plc->backward_period > 0U &&
                             plc->fill_pos == plc->fill_frames;
      plc->blend_hold[0] = plc->last[0];
      plc->blend_hold[1] = plc->last[1];
    }
    if (plc->blend_pos < PLC_SEAM_FRAMES) {
      const float w = float(plc->blend_pos + 1U) / float(PLC_SEAM_FRAMES + 1U);
      const uint32_t continued = plc->fill_pos + plc->blend_pos;
      int16_t fill[PLC_STRIDE] = {plc->blend_hold[0], plc->blend_hold[1]};
      if (!plc->blend_from_tail && continued < plc->fill_frames) {
        synth_frame(plc, continued, fill);
      }
      for (uint32_t ch = 0; ch < PLC_STRIDE; ch++) {
        /* The backward loop continued past the hole converges on the real
         * audio it was cut from. */
        const float source =
            plc->blend_from_tail
                ? backward_loop(plc, plc->blend_pos % plc->backward_period, ch)
                : float(fill[ch]);
        frame[ch] = clamp16((1.0f - w) * source + w * float(frame[ch]));
      }
      plc->blend_pos++;
      for (uint8_t ch = 0; ch < channels; ch++) {
        in[ch] = frame[ch];
      }
    }
    push_history(plc, frame);
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
