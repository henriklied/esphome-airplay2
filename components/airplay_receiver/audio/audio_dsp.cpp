// airplay_receiver output DSP -- RBJ biquad cascade on the playout path.
// See audio_dsp.h for the threading contract; it is the load-bearing part of
// this file.

#include "audio_dsp.h"

#include "esphome/core/log.h"

#include <atomic>
#include <cmath>
#include <cstring>

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

/// Normalized biquad (a0 divided out), transposed direct form II.
struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

/// A complete, self-consistent coefficient set. Two of these are double
/// buffered so the audio task never observes a half-written cascade.
struct CoeffBank {
  Biquad sections[AIRPLAY_DSP_MAX_FILTERS];
  size_t count = 0;
  float preamp_lin = 1.0f;
};

CoeffBank g_banks[2];
std::atomic<uint8_t> g_active_bank{0};
std::atomic<bool> g_enabled{true};

// Filter state, indexed [section][channel]. Deliberately outside the banks:
// it must survive a coefficient swap or every edit clicks.
float g_z1[AIRPLAY_DSP_MAX_FILTERS][CHANNELS];
float g_z2[AIRPLAY_DSP_MAX_FILTERS][CHANNELS];

// Configuration in human units. Writer-thread only.
AirPlayDspFilter g_filters[AIRPLAY_DSP_MAX_FILTERS];
size_t g_count = 0;
float g_preamp_db = 0.0f;
uint32_t g_sample_rate = 44100;

std::atomic<uint32_t> g_peak_samples{0};

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

/**
 * Rebuild the inactive bank from the human-unit config and publish it.
 *
 * State for sections that are newly in range is zeroed before the flip, while
 * the audio task still cannot reach them; sections that merely changed shape
 * keep their state so a live edit does not click.
 */
void rebuild_and_publish() {
  const uint8_t active = g_active_bank.load(std::memory_order_relaxed);
  const uint8_t target = active ^ 1u;
  CoeffBank &bank = g_banks[target];

  const size_t previous_count = g_banks[active].count;

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

  for (size_t i = previous_count; i < built; i++) {
    for (size_t ch = 0; ch < CHANNELS; ch++) {
      g_z1[i][ch] = 0.0f;
      g_z2[i][ch] = 0.0f;
    }
  }

  // Release: every write above must be visible before the audio task, running
  // on the other core, can observe the new index.
  g_active_bank.store(target, std::memory_order_release);
}

}  // namespace

void audio_dsp_set_sample_rate(uint32_t sample_rate) {
  if (sample_rate == 0 || sample_rate == g_sample_rate) {
    return;
  }
  g_sample_rate = sample_rate;
  rebuild_and_publish();
}

void audio_dsp_set_filters(const AirPlayDspFilter *filters, size_t count) {
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
  if (index >= g_count || out == nullptr) {
    return false;
  }
  *out = g_filters[index];
  return true;
}

size_t audio_dsp_get_filter_count(void) { return g_count; }

void audio_dsp_set_preamp_db(float preamp_db) {
  if (preamp_db == g_preamp_db) {
    return;
  }
  g_preamp_db = preamp_db;
  rebuild_and_publish();
}

float audio_dsp_get_preamp_db(void) { return g_preamp_db; }

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
  const CoeffBank &bank = g_banks[g_active_bank.load(std::memory_order_acquire)];
  return bank.count > 0 || bank.preamp_lin != 1.0f;
}

void audio_dsp_reset(void) {
  memset(g_z1, 0, sizeof(g_z1));
  memset(g_z2, 0, sizeof(g_z2));
}

void audio_dsp_process(int16_t *buf, size_t frames) {
  if (buf == nullptr || frames == 0 || !g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  // Acquire pairs with the release in rebuild_and_publish(): the bank contents
  // are guaranteed visible once the index is.
  const CoeffBank &bank = g_banks[g_active_bank.load(std::memory_order_acquire)];
  const size_t sections = bank.count;
  const float preamp = bank.preamp_lin;
  if (sections == 0 && preamp == 1.0f) {
    return;  // bypass is bit-exact
  }

  float peak = 0.0f;
  for (size_t i = 0; i < frames; i++) {
    for (size_t ch = 0; ch < CHANNELS; ch++) {
      float x = (float) buf[i * CHANNELS + ch] * preamp;
      for (size_t s = 0; s < sections; s++) {
        const Biquad &c = bank.sections[s];
        // Transposed direct form II: one multiply-add chain per section, and
        // the state is the pair that carries across samples.
        const float y = c.b0 * x + g_z1[s][ch];
        g_z1[s][ch] = c.b1 * x - c.a1 * y + g_z2[s][ch];
        g_z2[s][ch] = c.b2 * x - c.a2 * y;
        x = y;
      }
      const float magnitude = fabsf(x);
      if (magnitude > peak) {
        peak = magnitude;
      }
      // Round rather than truncate: truncation toward zero on a bipolar signal
      // is a half-LSB DC step at the zero crossing.
      float rounded = roundf(x);
      if (rounded > PCM_MAX) {
        rounded = PCM_MAX;
      } else if (rounded < PCM_MIN) {
        rounded = PCM_MIN;
      }
      buf[i * CHANNELS + ch] = (int16_t) rounded;
    }
  }

  // Held in raw sample units; audio_dsp_take_peak() scales to full scale.
  const uint32_t peak_samples = (uint32_t) peak;
  uint32_t seen = g_peak_samples.load(std::memory_order_relaxed);
  while (peak_samples > seen && !g_peak_samples.compare_exchange_weak(seen, peak_samples, std::memory_order_relaxed)) {
  }
}

float audio_dsp_take_peak(void) {
  return (float) g_peak_samples.exchange(0, std::memory_order_relaxed) / PCM_FULL_SCALE;
}

void audio_dsp_log_cascade(const char *tag, int level) {
  const CoeffBank &bank = g_banks[g_active_bank.load(std::memory_order_acquire)];
  if (g_count == 0 && g_preamp_db == 0.0f) {
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
}

}  // namespace airplay_receiver
}  // namespace esphome
