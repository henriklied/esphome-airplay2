// airplay_receiver output DSP -- RBJ biquad cascade plus the dynamic stages
// (loudness, harmonic bass, centre-locked stereo width, ambience, crosstalk
// cancellation, bass and
// full-band look-ahead limiters) on the playout
// path. See audio_dsp.h for the threading contract; it is the load-bearing
// part of this file.

#include "audio_dsp.h"

#include "esphome/core/log.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

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

/// Bass protection: 4th-order Butterworth high-pass as two biquads.
constexpr size_t PROTECTION_SECTIONS = 2;
constexpr float PROTECTION_Q[PROTECTION_SECTIONS] = {0.54119610f, 1.30656296f};

/// Width/ambience analysis: covariance smoothing time. Long enough to see a
/// source's direction across a note, short enough to follow a mix change.
constexpr float SPATIAL_COVARIANCE_MS = 30.0f;
/// Below this smoothed energy (sample units squared) the direction estimate is
/// noise; the previous one is kept.
constexpr float SPATIAL_ENERGY_FLOOR = 1.0f;
/// The direction of a centred source, and of the M/S width it reduces to.
constexpr float CENTRE_ANGLE = PI_F * 0.25f;

/// Ambience reverb: a 4-line feedback delay network. Mutually prime-ish
/// lengths spread the echo density; all are past ~30 ms so the reverb reads
/// as room, not as colouration of the direct sound.
constexpr size_t AMBIENCE_LINES = 4;
constexpr float AMBIENCE_LINE_MS[AMBIENCE_LINES] = {31.3f, 37.1f, 41.9f, 47.3f};
/// Holds the longest line at 48 kHz with margin; higher rates clamp.
constexpr uint32_t AMBIENCE_MAX_LINE = 2400;
constexpr float AMBIENCE_DECAY_DB = 60.0f;

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

  /// Analysis/processing band for width and ambience: above this high-pass.
  bool spatial_on = false;  // width_on || ambience_on
  Biquad spatial_high_pass;
  float spatial_smoothing = 0.0f;  // per-sample covariance coefficient
  bool width_on = false;
  /// width - 1: the extra ambient component added above the corner.
  float width_extra = 0.0f;
  /// 0: widen along the fixed side axis (plain M/S). 1: widen only what is
  /// orthogonal to the dominant source, wherever it is panned.
  float centre_lock = 0.0f;

  bool ambience_on = false;
  float *ambience_buffer = nullptr;  // AMBIENCE_LINES * AMBIENCE_MAX_LINE
  uint32_t ambience_length[AMBIENCE_LINES] = {};
  float ambience_feedback[AMBIENCE_LINES] = {};
  float ambience_damping = 0.0f;  // one-pole low-pass coefficient in the loop
  float ambience_wet = 0.0f;

  bool crosstalk_on = false;
  Biquad crosstalk_high_pass;
  Biquad crosstalk_low_pass;
  float crosstalk_amount = 0.0f;
  uint32_t crosstalk_delay_whole = 1;  // integer part of the delay, samples
  float crosstalk_delay_fraction = 0.0f;

  bool protection_on = false;
  Biquad protection[PROTECTION_SECTIONS];

  bool bass_limiter_on = false;
  Biquad crossover_low_pass;   // each run twice: Linkwitz-Riley 4th order
  Biquad crossover_high_pass;
  float bass_limiter_threshold = 0.0f;  // sample units
  float bass_limiter_release = 0.0f;

  bool limiter_on = false;
  /// Shared by both limiters; designed whether or not either is on.
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

struct SpatialState {
  BiquadState high_pass[CHANNELS];
  float cov_ll = 0.0f;
  float cov_rr = 0.0f;
  float cov_lr = 0.0f;
  float angle = CENTRE_ANGLE;  // dominant-source direction estimate
  float dominance = 0.0f;      // how much one source dominates, 0..1
  // Ambient axis at the end of the last block, for per-sample interpolation.
  float width_axis[CHANNELS] = {0.70710678f, -0.70710678f};
  float feed_axis[CHANNELS] = {0.70710678f, -0.70710678f};
};
SpatialState g_spatial;

struct AmbienceState {
  uint32_t pos[AMBIENCE_LINES] = {};
  float damped[AMBIENCE_LINES] = {};
  uint32_t length[AMBIENCE_LINES] = {};  // what the buffer was cleared for
};
AmbienceState g_ambience;

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
BiquadState g_protection_state[PROTECTION_SECTIONS][CHANNELS];
bool g_was_protection_on = false;

LimiterState g_limiter;
LimiterState g_bass_limiter;

/// Bass-limiter band split, per channel, and the delay that keeps the highs
/// aligned with the limited (delayed) lows.
struct CrossoverState {
  BiquadState low_pass[2][CHANNELS];
  BiquadState high_pass[2][CHANNELS];
  float high_delay[LIMITER_MAX_LOOKAHEAD][CHANNELS];
  uint32_t high_delay_pos = 0;
};
CrossoverState g_crossover;

// Previous block's stage switches, to clear a stage's state as it comes on.
bool g_was_loudness_on = false;
bool g_was_enhancer_on = false;
bool g_was_spatial_on = false;
bool g_was_crosstalk_on = false;

std::atomic<uint32_t> g_peak_samples{0};
/// Lowest limiter gain since the last take, as gain * LIMITER_GAIN_SCALE.
constexpr float LIMITER_GAIN_SCALE = 1000000.0f;
std::atomic<uint32_t> g_limiter_min_gain{(uint32_t) LIMITER_GAIN_SCALE};
std::atomic<uint32_t> g_bass_limiter_min_gain{(uint32_t) LIMITER_GAIN_SCALE};

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
float g_centre_lock = 0.0f;

float g_ambience_amount = 0.0f;
float g_ambience_decay_ms = 400.0f;
float g_ambience_damping_hz = 5000.0f;
float *g_ambience_buffer = nullptr;  // allocated once on first enable, never freed

/**
 * The reverb's delay memory, from PSRAM only. These boards build without
 * CONFIG_SPIRAM_USE_MALLOC, so plain malloc() is internal RAM -- and 38 KB of
 * that starved the decoder's 56 KB stack (ESP_ERR_NO_MEM, no audio) and then
 * WiFi itself (abort in a scan-result allocation). No internal fallback: no
 * ambience is the right failure, a dead board is not.
 */
float *allocate_ambience_buffer(size_t bytes) {
#ifdef ESP_PLATFORM
  return static_cast<float *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
  return static_cast<float *>(std::malloc(bytes));
#endif
}

float g_crosstalk_amount = 0.0f;
float g_crosstalk_delay_us = 60.0f;
float g_crosstalk_low_hz = 250.0f;
float g_crosstalk_high_hz = 5000.0f;

float g_protection_hz = 0.0f;

bool g_bass_limiter_enabled = false;
float g_bass_limiter_frequency_hz = 120.0f;
float g_bass_limiter_threshold_db = -6.0f;
float g_bass_limiter_release_ms = 200.0f;

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

void design_spatial(CoeffBank &bank) {
  bank.width_on = g_width != 1.0f;
  bank.width_extra = g_width - 1.0f;
  bank.centre_lock = g_centre_lock;

  bank.ambience_on = g_ambience_amount > 0.0f && g_ambience_buffer != nullptr;
  bank.ambience_buffer = g_ambience_buffer;
  bank.ambience_wet = g_ambience_amount;
  for (size_t i = 0; i < AMBIENCE_LINES; i++) {
    uint32_t length = (uint32_t) lroundf(AMBIENCE_LINE_MS[i] / MS_PER_S * (float) g_sample_rate);
    length = length < 2 ? 2 : (length > AMBIENCE_MAX_LINE ? AMBIENCE_MAX_LINE : length);
    bank.ambience_length[i] = length;
    // Per-pass gain that loses 60 dB over the decay time.
    const float line_ms = (float) length / (float) g_sample_rate * MS_PER_S;
    bank.ambience_feedback[i] = powf(10.0f, -AMBIENCE_DECAY_DB / 20.0f * line_ms / g_ambience_decay_ms);
  }
  bank.ambience_damping = 1.0f - expf(-2.0f * PI_F * g_ambience_damping_hz / (float) g_sample_rate);

  bank.spatial_on = (bank.width_on || bank.ambience_on) &&
                    design_section(AIRPLAY_DSP_HIGH_PASS, g_width_frequency_hz, DEFAULT_Q, 0.0f,
                                   &bank.spatial_high_pass);
  if (!bank.spatial_on) {
    bank.width_on = false;
    bank.ambience_on = false;
  }
  bank.spatial_smoothing = 1.0f - time_constant_coefficient(SPATIAL_COVARIANCE_MS);
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

float threshold_samples(float threshold_db) {
  return fminf(powf(10.0f, threshold_db / 20.0f) * PCM_FULL_SCALE, PCM_MAX);
}

void design_protection(CoeffBank &bank) {
  bank.protection_on = g_protection_hz > 0.0f;
  for (size_t i = 0; i < PROTECTION_SECTIONS && bank.protection_on; i++) {
    bank.protection_on =
        design_section(AIRPLAY_DSP_HIGH_PASS, g_protection_hz, PROTECTION_Q[i], 0.0f, &bank.protection[i]);
  }
}

void design_bass_limiter(CoeffBank &bank) {
  bank.bass_limiter_on =
      g_bass_limiter_enabled &&
      design_section(AIRPLAY_DSP_LOW_PASS, g_bass_limiter_frequency_hz, DEFAULT_Q, 0.0f, &bank.crossover_low_pass) &&
      design_section(AIRPLAY_DSP_HIGH_PASS, g_bass_limiter_frequency_hz, DEFAULT_Q, 0.0f, &bank.crossover_high_pass);
  bank.bass_limiter_threshold = threshold_samples(g_bass_limiter_threshold_db);
  bank.bass_limiter_release = time_constant_coefficient(g_bass_limiter_release_ms);
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
  bank.limiter_threshold = threshold_samples(g_limiter_threshold_db);
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
  design_spatial(bank);
  design_crosstalk(bank);
  design_limiter(bank);
  design_bass_limiter(bank);
  design_protection(bank);

  const uint32_t limiter_delay = bank.limiter_lookahead - 1;
  g_latency_frames.store((bank.limiter_on ? limiter_delay : 0) + (bank.bass_limiter_on ? limiter_delay : 0),
                         std::memory_order_relaxed);
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

void limiter_reset(LimiterState &s, uint32_t lookahead) {
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
  g_spatial = SpatialState{};
  g_ambience.length[0] = 0;  // forces sync_state_to() to clear the reverb
  g_crosstalk_state = CrosstalkState{};
  limiter_reset(g_limiter, g_limiter.lookahead);
  limiter_reset(g_bass_limiter, g_bass_limiter.lookahead);
  g_crossover = CrossoverState{};
  for (auto &section : g_protection_state) {
    for (auto &channel : section) {
      channel = BiquadState{};
    }
  }
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
  if (bank.spatial_on && !g_was_spatial_on) {
    g_spatial = SpatialState{};
  }
  if (bank.ambience_on &&
      memcmp(g_ambience.length, bank.ambience_length, sizeof(g_ambience.length)) != 0) {
    // New lengths (first enable, rate change, or reset): stale echoes at the
    // wrong spacing would ring on, so start from silence.
    memset(bank.ambience_buffer, 0, sizeof(float) * AMBIENCE_LINES * AMBIENCE_MAX_LINE);
    g_ambience = AmbienceState{};
    memcpy(g_ambience.length, bank.ambience_length, sizeof(g_ambience.length));
  }
  g_was_loudness_on = bank.loudness_on;
  g_was_enhancer_on = bank.enhancer_on;
  if (bank.crosstalk_on && !g_was_crosstalk_on) {
    g_crosstalk_state = CrosstalkState{};
  }
  g_was_spatial_on = bank.spatial_on;
  if (bank.protection_on && !g_was_protection_on) {
    for (auto &section : g_protection_state) {
      for (auto &channel : section) {
        channel = BiquadState{};
      }
    }
  }
  g_was_protection_on = bank.protection_on;
  g_was_crosstalk_on = bank.crosstalk_on;
  if (!bank.limiter_on) {
    g_limiter.lookahead = 0;  // a later enable starts clean
  } else if (bank.limiter_lookahead != g_limiter.lookahead) {
    limiter_reset(g_limiter, bank.limiter_lookahead);
  }
  if (!bank.bass_limiter_on) {
    g_bass_limiter.lookahead = 0;
  } else if (bank.limiter_lookahead != g_bass_limiter.lookahead) {
    limiter_reset(g_bass_limiter, bank.limiter_lookahead);
    g_crossover = CrossoverState{};
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

/// Unit vector orthogonal to a source at `angle`: the ambient axis.
inline void ambient_axis(float angle, float axis[CHANNELS]) {
  axis[0] = -sinf(angle);
  axis[1] = cosf(angle);
}

/**
 * Once per block: turn the smoothed covariance into the dominant-source angle
 * (principal axis of the 2x2 covariance) and the two ambient axes for this
 * block -- the estimated one, which feeds the ambience, and the one width uses,
 * blended from the fixed side axis toward it by `centre_lock`.
 */
void spatial_update_axes(const CoeffBank &bank, float width_axis[CHANNELS], float feed_axis[CHANNELS]) {
  SpatialState &sp = g_spatial;
  const float energy = sp.cov_ll + sp.cov_rr;
  if (energy > SPATIAL_ENERGY_FLOOR) {
    sp.angle = 0.5f * atan2f(2.0f * sp.cov_lr, sp.cov_ll - sp.cov_rr);
    // Eigenvalue spread over the sum: 1 for one source, 0 for diffuse sound
    // whose principal axis is meaningless. Steering scales with it, so diffuse
    // sound falls back to the plain side axis instead of a wandering one.
    const float difference = sp.cov_ll - sp.cov_rr;
    sp.dominance = fminf(sqrtf(difference * difference + 4.0f * sp.cov_lr * sp.cov_lr) / energy, 1.0f);
  }
  const float steer = sp.angle - CENTRE_ANGLE;
  ambient_axis(CENTRE_ANGLE + sp.dominance * steer, feed_axis);
  ambient_axis(CENTRE_ANGLE + bank.centre_lock * sp.dominance * steer, width_axis);
  // The axis sign is arbitrary; keep it continuous so interpolation between
  // blocks never passes through zero.
  if (width_axis[0] * sp.width_axis[0] + width_axis[1] * sp.width_axis[1] < 0.0f) {
    width_axis[0] = -width_axis[0];
    width_axis[1] = -width_axis[1];
  }
  if (feed_axis[0] * sp.feed_axis[0] + feed_axis[1] * sp.feed_axis[1] < 0.0f) {
    feed_axis[0] = -feed_axis[0];
    feed_axis[1] = -feed_axis[1];
  }
}

/// Small FDN reverb fed by the ambient component. Returns the wet pair.
inline void ambience_run(const CoeffBank &bank, float feed, float *wet_left, float *wet_right) {
  AmbienceState &a = g_ambience;
  float *const buffer = bank.ambience_buffer;
  float out[AMBIENCE_LINES];
  for (size_t i = 0; i < AMBIENCE_LINES; i++) {
    const float delayed = buffer[i * AMBIENCE_MAX_LINE + a.pos[i]];
    // Damping in the loop: highs die faster than lows, as in a furnished room.
    a.damped[i] += (delayed - a.damped[i]) * bank.ambience_damping;
    out[i] = a.damped[i];
  }
  // Orthonormal 4x4 Hadamard feedback: energy-preserving mixing between lines.
  const float g0 = out[0] * bank.ambience_feedback[0];
  const float g1 = out[1] * bank.ambience_feedback[1];
  const float g2 = out[2] * bank.ambience_feedback[2];
  const float g3 = out[3] * bank.ambience_feedback[3];
  const float mixed[AMBIENCE_LINES] = {
      0.5f * (g0 + g1 + g2 + g3),
      0.5f * (g0 - g1 + g2 - g3),
      0.5f * (g0 + g1 - g2 - g3),
      0.5f * (g0 - g1 - g2 + g3),
  };
  // Alternating input signs keep the feed from exciting one mode only.
  const float input = 0.5f * feed;
  const float input_sign[AMBIENCE_LINES] = {1.0f, -1.0f, 1.0f, -1.0f};
  for (size_t i = 0; i < AMBIENCE_LINES; i++) {
    buffer[i * AMBIENCE_MAX_LINE + a.pos[i]] = mixed[i] + input * input_sign[i];
    if (++a.pos[i] == bank.ambience_length[i]) {
      a.pos[i] = 0;
    }
  }
  // Two orthogonal output taps: the left and right tails are decorrelated.
  *wet_left = 0.5f * (out[0] + out[1] - out[2] - out[3]) * bank.ambience_wet;
  *wet_right = 0.5f * (out[0] - out[1] + out[2] - out[3]) * bank.ambience_wet;
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
inline float limiter_run(LimiterState &s, float threshold, float release, float inv_lookahead, float *left,
                         float *right) {
  const uint32_t lookahead = s.lookahead;

  const float peak = fmaxf(fabsf(*left), fabsf(*right));
  const float required = peak > threshold ? threshold / peak : 1.0f;

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

  s.released = held < s.released ? held : held + (s.released - held) * release;

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
  const float gain = s.box_sum * inv_lookahead;

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

/**
 * Split at the crossover (Linkwitz-Riley: the two bands sum back flat), limit
 * the lows on their own, delay the highs by the same look-ahead, and recombine.
 * Bass peaks then no longer pull the whole mix down. Returns the bass gain.
 */
inline float bass_limiter_run(const CoeffBank &bank, float *left, float *right) {
  CrossoverState &x = g_crossover;
  float low[CHANNELS] = {*left, *right};
  float high[CHANNELS] = {*left, *right};
  for (size_t ch = 0; ch < CHANNELS; ch++) {
    for (size_t stage = 0; stage < 2; stage++) {
      low[ch] = x.low_pass[stage][ch].run(bank.crossover_low_pass, low[ch]);
      high[ch] = x.high_pass[stage][ch].run(bank.crossover_high_pass, high[ch]);
    }
  }
  const float gain = limiter_run(g_bass_limiter, bank.bass_limiter_threshold, bank.bass_limiter_release,
                                 bank.limiter_inv_lookahead, &low[0], &low[1]);
  float *delayed = x.high_delay[x.high_delay_pos];
  *left = low[0] + delayed[0];
  *right = low[1] + delayed[1];
  delayed[0] = high[0];
  delayed[1] = high[1];
  if (++x.high_delay_pos == g_bass_limiter.lookahead - 1) {
    x.high_delay_pos = 0;
  }
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
  return bank.count > 0 || bank.preamp_lin != 1.0f || bank.loudness_on || bank.enhancer_on || bank.spatial_on || bank.crosstalk_on || bank.bass_limiter_on || bank.protection_on ||
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

void audio_dsp_set_centre_lock(float lock) {
  std::lock_guard<std::mutex> lock_guard(g_writer_mutex);
  g_centre_lock = fminf(fmaxf(lock, 0.0f), 1.0f);
  rebuild_and_publish();
}

void audio_dsp_set_ambience(float amount, float decay_ms, float damping_hz) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_ambience_amount = amount > 0.0f ? amount : 0.0f;
  g_ambience_decay_ms = decay_ms > 1.0f ? decay_ms : 1.0f;
  g_ambience_damping_hz = damping_hz;
  if (g_ambience_amount > 0.0f && g_ambience_buffer == nullptr) {
    // Off the audio path, once.
    g_ambience_buffer = allocate_ambience_buffer(sizeof(float) * AMBIENCE_LINES * AMBIENCE_MAX_LINE);
    if (g_ambience_buffer == nullptr) {
      ESP_LOGW(TAG, "Ambience disabled: no PSRAM for the reverb");
    }
  }
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

void audio_dsp_set_bass_protection(float frequency_hz) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_protection_hz = frequency_hz > 0.0f ? frequency_hz : 0.0f;
  rebuild_and_publish();
}

void audio_dsp_set_bass_limiter(bool enabled, float frequency_hz, float threshold_db, float release_ms) {
  std::lock_guard<std::mutex> lock(g_writer_mutex);
  g_bass_limiter_enabled = enabled;
  g_bass_limiter_frequency_hz = frequency_hz;
  g_bass_limiter_threshold_db = threshold_db < 0.0f ? threshold_db : 0.0f;
  g_bass_limiter_release_ms = release_ms;
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

namespace {
float take_reduction_db(std::atomic<uint32_t> &min_gain) {
  const uint32_t scaled = min_gain.exchange((uint32_t) LIMITER_GAIN_SCALE, std::memory_order_relaxed);
  const float gain = (float) scaled / LIMITER_GAIN_SCALE;
  return gain > 0.0f ? 20.0f * log10f(gain) : -120.0f;
}
}  // namespace

float audio_dsp_take_limiter_reduction_db(void) { return take_reduction_db(g_limiter_min_gain); }

float audio_dsp_take_bass_limiter_reduction_db(void) { return take_reduction_db(g_bass_limiter_min_gain); }

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

  // Spatial axes move once per block and are interpolated per sample, so a
  // change of direction never steps.
  float width_axis[CHANNELS] = {};
  float feed_axis[CHANNELS] = {};
  float width_step[CHANNELS] = {};
  float feed_step[CHANNELS] = {};
  if (bank.spatial_on) {
    float next_width[CHANNELS];
    float next_feed[CHANNELS];
    spatial_update_axes(bank, next_width, next_feed);
    const float per_frame = 1.0f / (float) frames;
    for (size_t ch = 0; ch < CHANNELS; ch++) {
      width_axis[ch] = g_spatial.width_axis[ch];
      feed_axis[ch] = g_spatial.feed_axis[ch];
      width_step[ch] = (next_width[ch] - width_axis[ch]) * per_frame;
      feed_step[ch] = (next_feed[ch] - feed_axis[ch]) * per_frame;
      g_spatial.width_axis[ch] = next_width[ch];
      g_spatial.feed_axis[ch] = next_feed[ch];
    }
  }
  float peak = 0.0f;
  float min_gain = 1.0f;
  float min_bass_gain = 1.0f;
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

    if (bank.spatial_on) {
      SpatialState &sp = g_spatial;
      for (size_t ch = 0; ch < CHANNELS; ch++) {
        width_axis[ch] += width_step[ch];
        feed_axis[ch] += feed_step[ch];
      }
      const float high_left = sp.high_pass[0].run(bank.spatial_high_pass, left);
      const float high_right = sp.high_pass[1].run(bank.spatial_high_pass, right);
      sp.cov_ll += (high_left * high_left - sp.cov_ll) * bank.spatial_smoothing;
      sp.cov_rr += (high_right * high_right - sp.cov_rr) * bank.spatial_smoothing;
      sp.cov_lr += (high_left * high_right - sp.cov_lr) * bank.spatial_smoothing;
      if (bank.width_on) {
        // Project onto the ambient axis and add (width - 1) more of it. On
        // the fixed side axis this is exactly side += (width - 1) * HP(side).
        const float ambient = high_left * width_axis[0] + high_right * width_axis[1];
        left += bank.width_extra * ambient * width_axis[0];
        right += bank.width_extra * ambient * width_axis[1];
      }
      if (bank.ambience_on) {
        float wet_left;
        float wet_right;
        ambience_run(bank, high_left * feed_axis[0] + high_right * feed_axis[1], &wet_left, &wet_right);
        left += wet_left;
        right += wet_right;
      }
    }

    if (bank.crosstalk_on) {
      crosstalk_run(bank, &left, &right);
    }

    // After every stage that adds bass, so no combination of loudness,
    // shelf and harmonics can push the deepest notes back past the corner.
    if (bank.protection_on) {
      for (size_t s = 0; s < PROTECTION_SECTIONS; s++) {
        left = g_protection_state[s][0].run(bank.protection[s], left);
        right = g_protection_state[s][1].run(bank.protection[s], right);
      }
    }

    if (bank.bass_limiter_on) {
      const float gain = bass_limiter_run(bank, &left, &right);
      if (gain < min_bass_gain) {
        min_bass_gain = gain;
      }
    }

    if (bank.limiter_on) {
      const float gain = limiter_run(g_limiter, bank.limiter_threshold, bank.limiter_release,
                                     bank.limiter_inv_lookahead, &left, &right);
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
  store_min(g_bass_limiter_min_gain, (uint32_t) (min_bass_gain * LIMITER_GAIN_SCALE));
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
    esp_log_printf_(level, tag, __LINE__, "    stereo width %.2f above %.0f Hz, centre lock %.2f", g_width,
                    g_width_frequency_hz, g_centre_lock);
  }
  if (bank.ambience_on) {
    esp_log_printf_(level, tag, __LINE__, "    ambience %.2f, decay %.0f ms, damping %.0f Hz", g_ambience_amount,
                    g_ambience_decay_ms, g_ambience_damping_hz);
  }
  if (bank.crosstalk_on) {
    esp_log_printf_(level, tag, __LINE__, "    crosstalk cancel %.2f, %.0f us, %.0f-%.0f Hz", bank.crosstalk_amount,
                    g_crosstalk_delay_us, g_crosstalk_low_hz, g_crosstalk_high_hz);
  }
  if (bank.protection_on) {
    esp_log_printf_(level, tag, __LINE__, "    bass protection 4th-order high-pass at %.0f Hz", g_protection_hz);
  }
  if (bank.bass_limiter_on) {
    esp_log_printf_(level, tag, __LINE__, "    bass limiter below %.0f Hz, %.1f dBFS, release %.0f ms",
                    g_bass_limiter_frequency_hz, g_bass_limiter_threshold_db, g_bass_limiter_release_ms);
  }
  if (bank.limiter_on) {
    esp_log_printf_(level, tag, __LINE__, "    limiter %.1f dBFS, release %.0f ms, look-ahead %u frames",
                    g_limiter_threshold_db, g_limiter_release_ms, (unsigned) bank.limiter_lookahead);
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
