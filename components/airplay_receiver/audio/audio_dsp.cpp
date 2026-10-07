// airplay_receiver output DSP -- RBJ biquad cascade plus the dynamic stages
// (loudness, harmonic bass, stereo width, crosstalk cancellation, look-ahead
// limiter) on the playout
// path. See audio_dsp.h for the threading contract; it is the load-bearing
// part of this file.

#include "audio_dsp.h"

#include "esphome/core/log.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_dsp";

namespace {

constexpr size_t CHANNELS = 2;
constexpr float PCM_FULL_SCALE = 32768.0f;
constexpr float PCM_MIN = -32768.0f;
constexpr float PCM_MAX = 32767.0f;
/// Butterworth Q. Used as the default when a config omits `q`.
constexpr float DEFAULT_Q = 0.7071067811865476f;
/// Below this the biquad design degenerates (w0 -> 0) and the section is
/// dropped rather than emitting coefficients full of NaN.
constexpr float MIN_FREQUENCY_HZ = 1.0f;
/// Nyquist guard: a corner this close to fs/2 has no usable response left.
constexpr float MAX_FREQUENCY_FRACTION = 0.49f;
constexpr float MIN_Q = 0.05f;
/// M_PI is not guaranteed by <cmath> under a strict -std=c++NN, so carry it.
constexpr float PI_F = 3.14159265358979323846f;
constexpr float MS_PER_S = 1000.0f;

/// Loudness is redesigned only when the lift moves by at least this much, so a
/// volume ramp does not churn coefficients on every step.
constexpr float LOUDNESS_STEP_DB = 0.1f;

/// Harmonic bass: the source band starts this fraction of the corner below it
/// (nothing useful lives under ~20 Hz, and rumble would only feed the envelope).
constexpr float ENHANCER_SOURCE_LOW_FRACTION = 0.25f;
constexpr float ENHANCER_SOURCE_LOW_MIN_HZ = 20.0f;
/// The 3rd harmonic of the corner frequency must survive the output band.
constexpr float ENHANCER_OUTPUT_HIGH_MULTIPLE = 4.0f;
/// Envelope release. Longer than the period of the lowest bass note so the
/// normalisation does not ripple at the fundamental (which would add
/// intermodulation of its own).
constexpr float ENHANCER_ENVELOPE_RELEASE_MS = 100.0f;
/// Below this envelope (sample units) the band is silence and normalising it
/// would only amplify noise.
constexpr float ENHANCER_ENVELOPE_FLOOR = 1.0f;
/// Harmonic mix: 2nd for weight, 3rd so the series (2f, 3f) implies f.
constexpr float ENHANCER_H2_WEIGHT = 0.6f;
constexpr float ENHANCER_H3_WEIGHT = 0.4f;

/// Crosstalk history: a power of two so the ring index is a mask. Holds the
/// longest delay (CROSSTALK_MAX_DELAY_US at 96 kHz) plus the interpolation tap.
constexpr uint32_t CROSSTALK_HISTORY = 32;
constexpr uint32_t CROSSTALK_HISTORY_MASK = CROSSTALK_HISTORY - 1;
constexpr float CROSSTALK_MAX_DELAY_SAMPLES = (float) (CROSSTALK_HISTORY - 3);
constexpr float CROSSTALK_MIN_DELAY_SAMPLES = 1.0f;
/// Recursion must decay: keep the loop gain strictly below unity.
constexpr float CROSSTALK_MAX_AMOUNT = 0.95f;
constexpr float US_PER_S = 1000000.0f;

/// Look-ahead long enough to ramp gain down over a full cycle of ~700 Hz, short
/// enough to be irrelevant to AirPlay sync.
constexpr float LIMITER_LOOKAHEAD_MS = 1.5f;
/// Covers the look-ahead at 96 kHz, plus the one extra entry the min-deque holds
/// between a push and the pop that follows it.
constexpr uint32_t LIMITER_MAX_LOOKAHEAD = 160;

/// Normalized biquad (a0 divided out), transposed direct form II.
struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

/// Transposed direct form II state for one biquad on one signal.
struct BiquadState {
  float z1 = 0.0f;
  float z2 = 0.0f;

  inline float run(const Biquad &c, float x) {
    const float y = c.b0 * x + this->z1;
    this->z1 = c.b1 * x - c.a1 * y + this->z2;
    this->z2 = c.b2 * x - c.a2 * y;
    return y;
  }
};

/// A complete, self-consistent coefficient set. Two of these are double
/// buffered so the audio task never observes a half-written cascade.
struct CoeffBank {
  Biquad sections[AIRPLAY_DSP_MAX_FILTERS];
  size_t count = 0;
  float preamp_lin = 1.0f;

  bool loudness_on = false;
  Biquad loudness;

  bool enhancer_on = false;
  Biquad enhancer_source_high_pass;
  Biquad enhancer_source_low_pass;  // run twice: 4th-order cut above the corner
  Biquad enhancer_output_high_pass;
  Biquad enhancer_output_low_pass;
  float enhancer_amount = 0.0f;
  float enhancer_release = 0.0f;

  bool width_on = false;
  Biquad width_side_high_pass;
  /// width - 1: the extra side signal added above the corner.
  float width_extra = 0.0f;

  bool crosstalk_on = false;
  Biquad crosstalk_high_pass;
  Biquad crosstalk_low_pass;
  float crosstalk_amount = 0.0f;
  uint32_t crosstalk_delay_whole = 1;  // integer part of the delay, samples
  float crosstalk_delay_fraction = 0.0f;

  bool limiter_on = false;
  uint32_t limiter_lookahead = 0;
  float limiter_inv_lookahead = 0.0f;
  float limiter_threshold = 0.0f;  // sample units
  float limiter_release = 0.0f;
};

CoeffBank g_banks[2];
std::atomic<uint8_t> g_active_bank{0};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_reset_requested{false};
std::atomic<uint32_t> g_latency_frames{0};
std::atomic<int32_t> g_volume_q15{32768};

// Serialises writers (main loop, setup, and the playback task's reclock). The
// audio path never takes it.
std::mutex g_writer_mutex;

// ---- Audio-task state ----------------------------------------------------
// Touched only by audio_dsp_process(). Resets requested from other tasks go
// through g_reset_requested, so nothing here is ever written from two tasks.

CoeffBank g_snapshot;
BiquadState g_cascade_state[AIRPLAY_DSP_MAX_FILTERS][CHANNELS];
BiquadState g_loudness_state[CHANNELS];

struct EnhancerState {
  BiquadState source_high_pass;
  BiquadState source_low_pass[2];
  BiquadState output_high_pass;
  BiquadState output_low_pass;
  float envelope = 0.0f;
};
EnhancerState g_enhancer_state;

BiquadState g_width_state;

/// Past crosstalk-stage outputs per channel, newest at `pos`, plus the band
/// filter state on each feedback path.
struct CrosstalkState {
  float history[CROSSTALK_HISTORY][CHANNELS];
  uint32_t pos = 0;
  BiquadState high_pass[CHANNELS];
  BiquadState low_pass[CHANNELS];
};
CrosstalkState g_crosstalk_state;

/**
 * Look-ahead limiter state. Gain computation, per frame n:
 *   required[n] = min(1, threshold / peak[n])
 *   held[n]     = min(required[n-L+1 .. n])          (sliding minimum, L frames)
 *   released[n] = held[n] if lower, else an exponential rise toward it
 *   gain[n]     = mean(released[n-L+1 .. n])         (box filter, L frames)
 * and the audio is delayed by L-1 frames. Every term of the mean is <= the
 * required gain of the sample leaving the delay line, so the output can never
 * exceed the threshold: the attack is a ramp that lands exactly on time.
 */
struct LimiterState {
  uint32_t lookahead = 0;
  float delay[LIMITER_MAX_LOOKAHEAD][CHANNELS];
  uint32_t delay_pos = 0;
  float box[LIMITER_MAX_LOOKAHEAD];
  uint32_t box_pos = 0;
  float box_sum = 0.0f;
  // Monotonic deque for the sliding minimum: values ascend from the front.
  float deque_value[LIMITER_MAX_LOOKAHEAD];
  uint32_t deque_frame[LIMITER_MAX_LOOKAHEAD];
  uint32_t deque_head = 0;
  uint32_t deque_size = 0;
  uint32_t frame = 0;
  float released = 1.0f;
};
LimiterState g_limiter;

// Previous block's stage switches, to clear a stage's state as it comes on.
bool g_was_loudness_on = false;
bool g_was_enhancer_on = false;
bool g_was_width_on = false;
bool g_was_crosstalk_on = false;

std::atomic<uint32_t> g_peak_samples{0};
/// Lowest limiter gain since the last take, as gain * LIMITER_GAIN_SCALE.
constexpr float LIMITER_GAIN_SCALE = 1000000.0f;
std::atomic<uint32_t> g_limiter_min_gain{(uint32_t) LIMITER_GAIN_SCALE};

// ---- Configuration in human units. Writer side, under g_writer_mutex. ----

AirPlayDspFilter g_filters[AIRPLAY_DSP_MAX_FILTERS];
size_t g_count = 0;
float g_preamp_db = 0.0f;
uint32_t g_sample_rate = 44100;

float g_loudness_frequency_hz = 100.0f;
float g_loudness_max_boost_db = 0.0f;
float g_loudness_range_db = 30.0f;
float g_loudness_boost_db = 0.0f;  // currently designed in

float g_enhancer_frequency_hz = 90.0f;
float g_enhancer_amount = 0.0f;

float g_width = 1.0f;
float g_width_frequency_hz = 300.0f;

float g_crosstalk_amount = 0.0f;
float g_crosstalk_delay_us = 60.0f;
float g_crosstalk_low_hz = 250.0f;
float g_crosstalk_high_hz = 5000.0f;

bool g_limiter_enabled = false;
float g_limiter_threshold_db = -1.0f;
float g_limiter_release_ms = 100.0f;

const char *filter_type_name(airplay_dsp_filter_type_t type) {
  switch (type) {
    case AIRPLAY_DSP_LOW_SHELF:
      return "low_shelf";
    case AIRPLAY_DSP_HIGH_SHELF:
      return "high_shelf";
    case AIRPLAY_DSP_HIGH_PASS:
      return "high_pass";
    case AIRPLAY_DSP_LOW_PASS:
      return "low_pass";
    case AIRPLAY_DSP_PEAKING:
      return "peaking";
    case AIRPLAY_DSP_NOTCH:
      return "notch";
  }
  return "?";
}

/**
 * RBJ Audio EQ Cookbook coefficients, normalized by a0.
 *
 * Shelves use the Q form (not the shelf-slope S form) because that is what
 * Music Assistant's DSP exposes, and these settings are ported from there.
 *
 * @return false if the section is degenerate and should be skipped.
 */
bool design_biquad(const AirPlayDspFilter &f, uint32_t sample_rate, Biquad *out) {
  const float nyquist_limit = (float) sample_rate * MAX_FREQUENCY_FRACTION;
  if (!(f.frequency_hz >= MIN_FREQUENCY_HZ) || f.frequency_hz > nyquist_limit) {
    return false;
  }
  const float q = (f.q >= MIN_Q) ? f.q : DEFAULT_Q;

  const float w0 = 2.0f * PI_F * f.frequency_hz / (float) sample_rate;
  const float cos_w0 = cosf(w0);
  const float sin_w0 = sinf(w0);
  const float alpha = sin_w0 / (2.0f * q);

  float b0, b1, b2, a0, a1, a2;

  switch (f.type) {
    case AIRPLAY_DSP_HIGH_PASS: {
      const float k = (1.0f + cos_w0) * 0.5f;
      b0 = k;
      b1 = -(1.0f + cos_w0);
      b2 = k;
      a0 = 1.0f + alpha;
      a1 = -2.0f * cos_w0;
      a2 = 1.0f - alpha;
      break;
    }
    case AIRPLAY_DSP_LOW_PASS: {
      const float k = (1.0f - cos_w0) * 0.5f;
      b0 = k;
      b1 = 1.0f - cos_w0;
      b2 = k;
      a0 = 1.0f + alpha;
      a1 = -2.0f * cos_w0;
      a2 = 1.0f - alpha;
      break;
    }
    case AIRPLAY_DSP_NOTCH: {
      b0 = 1.0f;
      b1 = -2.0f * cos_w0;
      b2 = 1.0f;
      a0 = 1.0f + alpha;
      a1 = -2.0f * cos_w0;
      a2 = 1.0f - alpha;
      break;
    }
    case AIRPLAY_DSP_PEAKING: {
      const float amp = powf(10.0f, f.gain_db / 40.0f);
      b0 = 1.0f + alpha * amp;
      b1 = -2.0f * cos_w0;
      b2 = 1.0f - alpha * amp;
      a0 = 1.0f + alpha / amp;
      a1 = -2.0f * cos_w0;
      a2 = 1.0f - alpha / amp;
      break;
    }
    case AIRPLAY_DSP_LOW_SHELF: {
      const float amp = powf(10.0f, f.gain_db / 40.0f);
      const float two_sqrt_a_alpha = 2.0f * sqrtf(amp) * alpha;
      b0 = amp * ((amp + 1.0f) - (amp - 1.0f) * cos_w0 + two_sqrt_a_alpha);
      b1 = 2.0f * amp * ((amp - 1.0f) - (amp + 1.0f) * cos_w0);
      b2 = amp * ((amp + 1.0f) - (amp - 1.0f) * cos_w0 - two_sqrt_a_alpha);
      a0 = (amp + 1.0f) + (amp - 1.0f) * cos_w0 + two_sqrt_a_alpha;
      a1 = -2.0f * ((amp - 1.0f) + (amp + 1.0f) * cos_w0);
      a2 = (amp + 1.0f) + (amp - 1.0f) * cos_w0 - two_sqrt_a_alpha;
      break;
    }
    case AIRPLAY_DSP_HIGH_SHELF: {
      const float amp = powf(10.0f, f.gain_db / 40.0f);
      const float two_sqrt_a_alpha = 2.0f * sqrtf(amp) * alpha;
      b0 = amp * ((amp + 1.0f) + (amp - 1.0f) * cos_w0 + two_sqrt_a_alpha);
      b1 = -2.0f * amp * ((amp - 1.0f) + (amp + 1.0f) * cos_w0);
      b2 = amp * ((amp + 1.0f) + (amp - 1.0f) * cos_w0 - two_sqrt_a_alpha);
      a0 = (amp + 1.0f) - (amp - 1.0f) * cos_w0 + two_sqrt_a_alpha;
      a1 = 2.0f * ((amp - 1.0f) - (amp + 1.0f) * cos_w0);
      a2 = (amp + 1.0f) - (amp - 1.0f) * cos_w0 - two_sqrt_a_alpha;
      break;
    }
    default:
      return false;
  }

  if (a0 == 0.0f || !std::isfinite(a0)) {
    return false;
  }
  out->b0 = b0 / a0;
  out->b1 = b1 / a0;
  out->b2 = b2 / a0;
  out->a1 = a1 / a0;
  out->a2 = a2 / a0;
  return std::isfinite(out->b0) && std::isfinite(out->b1) && std::isfinite(out->b2) &&
         std::isfinite(out->a1) && std::isfinite(out->a2);
}


bool design_section(airplay_dsp_filter_type_t type, float frequency_hz, float q, float gain_db, Biquad *out) {
  AirPlayDspFilter filter;
  filter.type = type;
  filter.frequency_hz = frequency_hz;
  filter.q = q;
  filter.gain_db = gain_db;
  return design_biquad(filter, g_sample_rate, out);
}

/// Per-sample smoothing coefficient for an exponential with time constant `ms`.
float time_constant_coefficient(float ms) {
  const float samples = ms / MS_PER_S * (float) g_sample_rate;
  return samples > 0.0f ? expf(-1.0f / samples) : 0.0f;
}

void design_loudness(CoeffBank &bank) {
  bank.loudness_on = g_loudness_max_boost_db != 0.0f &&
                     design_section(AIRPLAY_DSP_LOW_SHELF, g_loudness_frequency_hz, DEFAULT_Q, g_loudness_boost_db,
                                    &bank.loudness);
}

void design_enhancer(CoeffBank &bank) {
  bank.enhancer_on = false;
  if (g_enhancer_amount <= 0.0f) {
    return;
  }
  const float source_low_hz =
      fmaxf(g_enhancer_frequency_hz * ENHANCER_SOURCE_LOW_FRACTION, ENHANCER_SOURCE_LOW_MIN_HZ);
  const float output_high_hz = fminf(g_enhancer_frequency_hz * ENHANCER_OUTPUT_HIGH_MULTIPLE,
                                     (float) g_sample_rate * MAX_FREQUENCY_FRACTION * 0.9f);
  bank.enhancer_on =
      design_section(AIRPLAY_DSP_HIGH_PASS, source_low_hz, DEFAULT_Q, 0.0f, &bank.enhancer_source_high_pass) &&
      design_section(AIRPLAY_DSP_LOW_PASS, g_enhancer_frequency_hz, DEFAULT_Q, 0.0f, &bank.enhancer_source_low_pass) &&
      design_section(AIRPLAY_DSP_HIGH_PASS, g_enhancer_frequency_hz, DEFAULT_Q, 0.0f,
                     &bank.enhancer_output_high_pass) &&
      design_section(AIRPLAY_DSP_LOW_PASS, output_high_hz, DEFAULT_Q, 0.0f, &bank.enhancer_output_low_pass);
  bank.enhancer_amount = g_enhancer_amount;
  bank.enhancer_release = time_constant_coefficient(ENHANCER_ENVELOPE_RELEASE_MS);
}

void design_width(CoeffBank &bank) {
  bank.width_on = g_width != 1.0f &&
                  design_section(AIRPLAY_DSP_HIGH_PASS, g_width_frequency_hz, DEFAULT_Q, 0.0f,
                                 &bank.width_side_high_pass);
  bank.width_extra = g_width - 1.0f;
}

void design_crosstalk(CoeffBank &bank) {
  bank.crosstalk_on =
      g_crosstalk_amount > 0.0f &&
      design_section(AIRPLAY_DSP_HIGH_PASS, g_crosstalk_low_hz, DEFAULT_Q, 0.0f, &bank.crosstalk_high_pass) &&
      design_section(AIRPLAY_DSP_LOW_PASS, g_crosstalk_high_hz, DEFAULT_Q, 0.0f, &bank.crosstalk_low_pass);
  bank.crosstalk_amount = fminf(g_crosstalk_amount, CROSSTALK_MAX_AMOUNT);
  const float delay_samples =
      fminf(fmaxf(g_crosstalk_delay_us / US_PER_S * (float) g_sample_rate, CROSSTALK_MIN_DELAY_SAMPLES),
            CROSSTALK_MAX_DELAY_SAMPLES);
  bank.crosstalk_delay_whole = (uint32_t) delay_samples;
  bank.crosstalk_delay_fraction = delay_samples - (float) bank.crosstalk_delay_whole;
}

void design_limiter(CoeffBank &bank) {
  bank.limiter_on = g_limiter_enabled;
  uint32_t lookahead = (uint32_t) lroundf(LIMITER_LOOKAHEAD_MS / MS_PER_S * (float) g_sample_rate);
  if (lookahead < 2) {
    lookahead = 2;
  } else if (lookahead > LIMITER_MAX_LOOKAHEAD - 1) {
    lookahead = LIMITER_MAX_LOOKAHEAD - 1;
  }
  bank.limiter_lookahead = lookahead;
  bank.limiter_inv_lookahead = 1.0f / (float) lookahead;
  bank.limiter_threshold = fminf(powf(10.0f, g_limiter_threshold_db / 20.0f) * PCM_FULL_SCALE, PCM_MAX);
  bank.limiter_release = time_constant_coefficient(g_limiter_release_ms);
}

/**
 * Rebuild the inactive bank from the human-unit config and publish it. Caller
 * holds g_writer_mutex.
 *
 * Filter state is not touched here: it belongs to the audio task, which clears
 * whatever a new bank brings into use (see audio_dsp_process()).
 */
void rebuild_and_publish() {
  const uint8_t active = g_active_bank.load(std::memory_order_relaxed);
  const uint8_t target = active ^ 1u;
  CoeffBank &bank = g_banks[target];

  size_t built = 0;
  for (size_t i = 0; i < g_count && built < AIRPLAY_DSP_MAX_FILTERS; i++) {
    Biquad section;
    if (!design_biquad(g_filters[i], g_sample_rate, &section)) {
      ESP_LOGW(TAG, "Skipping filter %u (%s %.1f Hz Q %.2f): degenerate at %lu Hz", (unsigned) i,
               filter_type_name(g_filters[i].type), g_filters[i].frequency_hz, g_filters[i].q,
               (unsigned long) g_sample_rate);
      continue;
    }
    bank.sections[built++] = section;
  }
  bank.count = built;
  bank.preamp_lin = powf(10.0f, g_preamp_db / 20.0f);
  design_loudness(bank);
  design_enhancer(bank);
  design_width(bank);
  design_crosstalk(bank);
  design_limiter(bank);

  g_latency_frames.store(bank.limiter_on ? bank.limiter_lookahead - 1 : 0, std::memory_order_relaxed);
  // Release: every write above must be visible before the audio task, running
  // on the other core, can observe the new index.
  g_active_bank.store(target, std::memory_order_release);
}

float loudness_target_db(int32_t volume_q15) {
  if (g_loudness_max_boost_db == 0.0f) {
    return 0.0f;
  }
  float fraction = 1.0f;
  if (volume_q15 > 0) {
    const float volume_db = 20.0f * log10f((float) volume_q15 / PCM_FULL_SCALE);
    fraction = fminf(fmaxf(-volume_db / g_loudness_range_db, 0.0f), 1.0f);
  }
  return g_loudness_max_boost_db * fraction;
}

// ---- Audio task ------------------------------------------------------------

void limiter_reset(uint32_t lookahead) {
  LimiterState &s = g_limiter;
  s.lookahead = lookahead;
  memset(s.delay, 0, sizeof(s.delay));
  s.delay_pos = 0;
  for (uint32_t i = 0; i < LIMITER_MAX_LOOKAHEAD; i++) {
    s.box[i] = 1.0f;
  }
  s.box_pos = 0;
  s.box_sum = (float) lookahead;
  s.deque_head = 0;
  s.deque_size = 0;
  s.frame = 0;
  s.released = 1.0f;
}

void clear_all_state() {
  for (auto &section : g_cascade_state) {
    for (auto &channel : section) {
      channel = BiquadState{};
    }
  }
  for (auto &channel : g_loudness_state) {
    channel = BiquadState{};
  }
  g_enhancer_state = EnhancerState{};
  g_width_state = BiquadState{};
  g_crosstalk_state = CrosstalkState{};
  limiter_reset(g_limiter.lookahead);
}

size_t g_state_count = 0;

/// Clear the state of anything the new snapshot brings into use.
void sync_state_to(const CoeffBank &bank) {
  for (size_t i = g_state_count; i < bank.count; i++) {
    for (auto &channel : g_cascade_state[i]) {
      channel = BiquadState{};
    }
  }
  g_state_count = bank.count;
  if (bank.loudness_on && !g_was_loudness_on) {
    for (auto &channel : g_loudness_state) {
      channel = BiquadState{};
    }
  }
  if (bank.enhancer_on && !g_was_enhancer_on) {
    g_enhancer_state = EnhancerState{};
  }
  if (bank.width_on && !g_was_width_on) {
    g_width_state = BiquadState{};
  }
  g_was_loudness_on = bank.loudness_on;
  g_was_enhancer_on = bank.enhancer_on;
  if (bank.crosstalk_on && !g_was_crosstalk_on) {
    g_crosstalk_state = CrosstalkState{};
  }
  g_was_width_on = bank.width_on;
  g_was_crosstalk_on = bank.crosstalk_on;
  if (!bank.limiter_on) {
    g_limiter.lookahead = 0;  // a later enable starts clean
  } else if (bank.limiter_lookahead != g_limiter.lookahead) {
    limiter_reset(bank.limiter_lookahead);
  }
}

/// Synthesized harmonics of the sub-corner band of `mono`, ready to mix in.
inline float enhancer_run(const CoeffBank &bank, float mono) {
  EnhancerState &e = g_enhancer_state;
  float band = e.source_high_pass.run(bank.enhancer_source_high_pass, mono);
  band = e.source_low_pass[0].run(bank.enhancer_source_low_pass, band);
  band = e.source_low_pass[1].run(bank.enhancer_source_low_pass, band);

  // Peak envelope: instant attack keeps |band / envelope| <= 1, which is the
  // domain where Chebyshev T2/T3 turn a sinusoid into exactly its 2nd/3rd
  // harmonic.
  const float magnitude = fabsf(band);
  const float decayed = e.envelope * bank.enhancer_release;
  e.envelope = magnitude > decayed ? magnitude : decayed;

  float generated = 0.0f;
  if (e.envelope > ENHANCER_ENVELOPE_FLOOR) {
    const float u = band / e.envelope;
    const float u2 = u * u;
    const float t2 = 2.0f * u2 - 1.0f;
    const float t3 = u * (4.0f * u2 - 3.0f);
    generated = e.envelope * (ENHANCER_H2_WEIGHT * t2 + ENHANCER_H3_WEIGHT * t3);
  }
  // The high-pass also strips T2's DC term (it follows the envelope).
  generated = e.output_high_pass.run(bank.enhancer_output_high_pass, generated);
  generated = e.output_low_pass.run(bank.enhancer_output_low_pass, generated);
  return generated * bank.enhancer_amount;
}

/// Output of channel `channel`, `delay` samples (whole + fraction) before the
/// one about to be written. Linear interpolation: its slight treble loss is
/// below the band's own low-pass.
inline float crosstalk_past(uint32_t channel, uint32_t whole, float fraction) {
  const CrosstalkState &x = g_crosstalk_state;
  // The newest stored output is one sample back, at pos.
  const float near = x.history[(x.pos + 1 - whole) & CROSSTALK_HISTORY_MASK][channel];
  const float far = x.history[(x.pos - whole) & CROSSTALK_HISTORY_MASK][channel];
  return near + (far - near) * fraction;
}

inline void crosstalk_run(const CoeffBank &bank, float *left, float *right) {
  CrosstalkState &x = g_crosstalk_state;
  float from_right = crosstalk_past(1, bank.crosstalk_delay_whole, bank.crosstalk_delay_fraction);
  float from_left = crosstalk_past(0, bank.crosstalk_delay_whole, bank.crosstalk_delay_fraction);
  from_right = x.low_pass[0].run(bank.crosstalk_low_pass, x.high_pass[0].run(bank.crosstalk_high_pass, from_right));
  from_left = x.low_pass[1].run(bank.crosstalk_low_pass, x.high_pass[1].run(bank.crosstalk_high_pass, from_left));
  *left -= bank.crosstalk_amount * from_right;
  *right -= bank.crosstalk_amount * from_left;
  x.pos = (x.pos + 1) & CROSSTALK_HISTORY_MASK;
  x.history[x.pos][0] = *left;
  x.history[x.pos][1] = *right;
}

/// See LimiterState for the algorithm. Returns the gain applied.
inline float limiter_run(const CoeffBank &bank, float *left, float *right) {
  LimiterState &s = g_limiter;
  const uint32_t lookahead = s.lookahead;

  const float peak = fmaxf(fabsf(*left), fabsf(*right));
  const float required = peak > bank.limiter_threshold ? bank.limiter_threshold / peak : 1.0f;

  while (s.deque_size > 0) {
    const uint32_t back = (s.deque_head + s.deque_size - 1) % LIMITER_MAX_LOOKAHEAD;
    if (s.deque_value[back] < required) {
      break;
    }
    s.deque_size--;
  }
  const uint32_t slot = (s.deque_head + s.deque_size) % LIMITER_MAX_LOOKAHEAD;
  s.deque_value[slot] = required;
  s.deque_frame[slot] = s.frame;
  s.deque_size++;
  // Frame indices are unsigned and wrap; the difference is still the age.
  if (s.frame - s.deque_frame[s.deque_head] >= lookahead) {
    s.deque_head = (s.deque_head + 1) % LIMITER_MAX_LOOKAHEAD;
    s.deque_size--;
  }
  const float held = s.deque_value[s.deque_head];

  s.released = held < s.released ? held : held + (s.released - held) * bank.limiter_release;

  s.box_sum += s.released - s.box[s.box_pos];
  s.box[s.box_pos] = s.released;
  if (++s.box_pos == lookahead) {
    // Re-sum once per window so float error in the running sum cannot drift.
    s.box_pos = 0;
    float sum = 0.0f;
    for (uint32_t i = 0; i < lookahead; i++) {
      sum += s.box[i];
    }
    s.box_sum = sum;
  }
  const float gain = s.box_sum * bank.limiter_inv_lookahead;

  float *delayed = s.delay[s.delay_pos];
  const float out_left = delayed[0] * gain;
  const float out_right = delayed[1] * gain;
  delayed[0] = *left;
  delayed[1] = *right;
  if (++s.delay_pos == lookahead - 1) {
    s.delay_pos = 0;
  }
  s.frame++;
  *left = out_left;
  *right = out_right;
  return gain;
}

inline int16_t to_pcm(float x) {
  // Round rather than truncate: truncation toward zero on a bipolar signal is
  // a half-LSB DC step at the zero crossing.
  float rounded = roundf(x);
  if (rounded > PCM_MAX) {
    rounded = PCM_MAX;
  } else if (rounded < PCM_MIN) {
    rounded = PCM_MIN;
  }
  return (int16_t) rounded;
}

bool bank_is_active(const CoeffBank &bank) {
  return bank.count > 0 || bank.preamp_lin != 1.0f || bank.loudness_on || bank.enhancer_on || bank.width_on || bank.crosstalk_on ||
         bank.limiter_on;
}

void store_min(std::atomic<uint32_t> &target, uint32_t value) {
  uint32_t seen = target.load(std::memory_order_relaxed);
  while (value < seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

void store_max(std::atomic<uint32_t> &target, uint32_t value) {
  uint32_t seen = target.load(std::memory_order_relaxed);
  while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

void audio_dsp_set_sample_rate(uint32_t sample_rate) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  if (sample_rate == 0 || sample_rate == g_sample_rate) {
    return;
  }
  g_sample_rate = sample_rate;
  rebuild_and_publish();
}

void audio_dsp_set_filters(const AirPlayDspFilter *filters, size_t count) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  if (count > AIRPLAY_DSP_MAX_FILTERS) {
    ESP_LOGW(TAG, "%u filters configured, keeping the first %u", (unsigned) count,
             (unsigned) AIRPLAY_DSP_MAX_FILTERS);
    count = AIRPLAY_DSP_MAX_FILTERS;
  }
  for (size_t i = 0; i < count; i++) {
    g_filters[i] = filters[i];
  }
  g_count = count;
  rebuild_and_publish();
}

void audio_dsp_set_filter(size_t index, const AirPlayDspFilter &filter) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  if (index >= AIRPLAY_DSP_MAX_FILTERS) {
    ESP_LOGW(TAG, "Filter index %u out of range (max %u)", (unsigned) index, (unsigned) AIRPLAY_DSP_MAX_FILTERS - 1);
    return;
  }
  g_filters[index] = filter;
  if (index >= g_count) {
    g_count = index + 1;
  }
  rebuild_and_publish();
}

bool audio_dsp_get_filter(size_t index, AirPlayDspFilter *out) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  if (index >= g_count || out == nullptr) {
    return false;
  }
  *out = g_filters[index];
  return true;
}

size_t audio_dsp_get_filter_count(void) { return g_count; }

void audio_dsp_set_preamp_db(float preamp_db) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  if (preamp_db == g_preamp_db) {
    return;
  }
  g_preamp_db = preamp_db;
  rebuild_and_publish();
}

float audio_dsp_get_preamp_db(void) { return g_preamp_db; }

void audio_dsp_set_loudness(float frequency_hz, float max_boost_db, float range_db) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_loudness_frequency_hz = frequency_hz;
  g_loudness_max_boost_db = max_boost_db;
  g_loudness_range_db = range_db > 0.0f ? range_db : 1.0f;
  g_loudness_boost_db = loudness_target_db(g_volume_q15.load(std::memory_order_relaxed));
  rebuild_and_publish();
}

void audio_dsp_set_volume_q15(int32_t volume_q15) { g_volume_q15.store(volume_q15, std::memory_order_relaxed); }

void audio_dsp_service(void) {
  static int32_t serviced_volume_q15 = -1;
  const int32_t volume_q15 = g_volume_q15.load(std::memory_order_relaxed);
  if (volume_q15 == serviced_volume_q15) {
    return;
  }
  serviced_volume_q15 = volume_q15;
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  const float target_db = loudness_target_db(volume_q15);
  if (fabsf(target_db - g_loudness_boost_db) < LOUDNESS_STEP_DB) {
    return;
  }
  g_loudness_boost_db = target_db;
  rebuild_and_publish();
}

float audio_dsp_get_loudness_boost_db(void) { return g_loudness_boost_db; }

void audio_dsp_set_bass_enhancer(float frequency_hz, float amount) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_enhancer_frequency_hz = frequency_hz;
  g_enhancer_amount = amount > 0.0f ? amount : 0.0f;
  rebuild_and_publish();
}

void audio_dsp_set_stereo_width(float width, float frequency_hz) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_width = width > 0.0f ? width : 0.0f;
  g_width_frequency_hz = frequency_hz;
  rebuild_and_publish();
}

void audio_dsp_set_crosstalk(float amount, float delay_us, float low_hz, float high_hz) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_crosstalk_amount = amount > 0.0f ? amount : 0.0f;
  g_crosstalk_delay_us = delay_us;
  g_crosstalk_low_hz = low_hz;
  g_crosstalk_high_hz = high_hz;
  rebuild_and_publish();
}

void audio_dsp_set_limiter(bool enabled, float threshold_db, float release_ms) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_limiter_enabled = enabled;
  g_limiter_threshold_db = threshold_db < 0.0f ? threshold_db : 0.0f;
  g_limiter_release_ms = release_ms;
  rebuild_and_publish();
}

uint32_t audio_dsp_get_latency_frames(void) {
  return g_enabled.load(std::memory_order_relaxed) ? g_latency_frames.load(std::memory_order_relaxed) : 0;
}

float audio_dsp_take_limiter_reduction_db(void) {
  const uint32_t scaled = g_limiter_min_gain.exchange((uint32_t) LIMITER_GAIN_SCALE, std::memory_order_relaxed);
  const float gain = (float) scaled / LIMITER_GAIN_SCALE;
  return gain > 0.0f ? 20.0f * log10f(gain) : -120.0f;
}

void audio_dsp_set_enabled(bool enabled) {
  if (g_enabled.exchange(enabled, std::memory_order_relaxed) != enabled && enabled) {
    audio_dsp_reset();  // re-entering with stale state would thump
  }
}

bool audio_dsp_is_enabled(void) { return g_enabled.load(std::memory_order_relaxed); }

bool audio_dsp_is_active(void) {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return false;
  }
  return bank_is_active(g_banks[g_active_bank.load(std::memory_order_acquire)]);
}

void audio_dsp_reset(void) { g_reset_requested.store(true, std::memory_order_relaxed); }

void audio_dsp_process(int16_t *buf, size_t frames) {
  if (buf == nullptr || frames == 0 || !g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  // Acquire pairs with the release in rebuild_and_publish(): the bank contents
  // are guaranteed visible once the index is. The copy pins one bank for the
  // whole block, so a writer flipping twice meanwhile cannot change it mid-way.
  g_snapshot = g_banks[g_active_bank.load(std::memory_order_acquire)];
  const CoeffBank &bank = g_snapshot;
  if (!bank_is_active(bank)) {
    return;  // bypass is bit-exact
  }
  if (g_reset_requested.exchange(false, std::memory_order_relaxed)) {
    clear_all_state();
  }
  sync_state_to(bank);

  const size_t sections = bank.count;
  const float preamp = bank.preamp_lin;
  float peak = 0.0f;
  float min_gain = 1.0f;
  for (size_t i = 0; i < frames; i++) {
    float left = (float) buf[i * CHANNELS] * preamp;
    float right = (float) buf[i * CHANNELS + 1] * preamp;

    const float harmonics = bank.enhancer_on ? enhancer_run(bank, (left + right) * 0.5f) : 0.0f;

    for (size_t s = 0; s < sections; s++) {
      left = g_cascade_state[s][0].run(bank.sections[s], left);
      right = g_cascade_state[s][1].run(bank.sections[s], right);
    }
    if (bank.loudness_on) {
      left = g_loudness_state[0].run(bank.loudness, left);
      right = g_loudness_state[1].run(bank.loudness, right);
    }
    left += harmonics;
    right += harmonics;

    if (bank.width_on) {
      const float mid = (left + right) * 0.5f;
      float side = (left - right) * 0.5f;
      side += bank.width_extra * g_width_state.run(bank.width_side_high_pass, side);
      left = mid + side;
      right = mid - side;
    }

    if (bank.crosstalk_on) {
      crosstalk_run(bank, &left, &right);
    }

    if (bank.limiter_on) {
      const float gain = limiter_run(bank, &left, &right);
      if (gain < min_gain) {
        min_gain = gain;
      }
    }

    peak = fmaxf(peak, fmaxf(fabsf(left), fabsf(right)));
    buf[i * CHANNELS] = to_pcm(left);
    buf[i * CHANNELS + 1] = to_pcm(right);
  }

  // Held in raw sample units; audio_dsp_take_peak() scales to full scale.
  store_max(g_peak_samples, (uint32_t) peak);
  store_min(g_limiter_min_gain, (uint32_t) (min_gain * LIMITER_GAIN_SCALE));
}

float audio_dsp_take_peak(void) {
  return (float) g_peak_samples.exchange(0, std::memory_order_relaxed) / PCM_FULL_SCALE;
}

void audio_dsp_log_cascade(const char *tag, int level) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  const CoeffBank &bank = g_banks[g_active_bank.load(std::memory_order_acquire)];
  if (!bank_is_active(bank) && g_count == 0) {
    esp_log_printf_(level, tag, __LINE__, "  dsp: none");
    return;
  }
  esp_log_printf_(level, tag, __LINE__, "  dsp: %s, preamp %.1f dB, %u/%u sections at %lu Hz",
                  g_enabled.load(std::memory_order_relaxed) ? "enabled" : "bypassed", g_preamp_db,
                  (unsigned) bank.count, (unsigned) g_count, (unsigned long) g_sample_rate);
  // Index is what the runtime setters address, so print it -- a YAML reorder is
  // otherwise invisible until something sounds wrong.
  for (size_t i = 0; i < g_count; i++) {
    const AirPlayDspFilter &f = g_filters[i];
    esp_log_printf_(level, tag, __LINE__, "    [%u] %s %.0f Hz Q %.2f %+.1f dB", (unsigned) i,
                    filter_type_name(f.type), f.frequency_hz, f.q, f.gain_db);
  }
  if (bank.loudness_on) {
    esp_log_printf_(level, tag, __LINE__, "    loudness %.0f Hz, up to %+.1f dB over %.0f dB (now %+.1f dB)",
                    g_loudness_frequency_hz, g_loudness_max_boost_db, g_loudness_range_db, g_loudness_boost_db);
  }
  if (bank.enhancer_on) {
    esp_log_printf_(level, tag, __LINE__, "    bass enhancer below %.0f Hz, amount %.2f", g_enhancer_frequency_hz,
                    g_enhancer_amount);
  }
  if (bank.width_on) {
    esp_log_printf_(level, tag, __LINE__, "    stereo width %.2f above %.0f Hz", g_width, g_width_frequency_hz);
  }
  if (bank.crosstalk_on) {
    esp_log_printf_(level, tag, __LINE__, "    crosstalk cancel %.2f, %.0f us, %.0f-%.0f Hz", bank.crosstalk_amount,
                    g_crosstalk_delay_us, g_crosstalk_low_hz, g_crosstalk_high_hz);
  }
  if (bank.limiter_on) {
    esp_log_printf_(level, tag, __LINE__, "    limiter %.1f dBFS, release %.0f ms, look-ahead %u frames",
                    g_limiter_threshold_db, g_limiter_release_ms, (unsigned) bank.limiter_lookahead);
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
