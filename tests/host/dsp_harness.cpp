// Host checks for the dynamic stages in audio/audio_dsp.cpp: limiter ceiling,
// latency and release, volume-following loudness, harmonic bass, stereo width
// crosstalk cancellation and the bass-band limiter. Exits non-zero with a message on the first failure. Driven by
// tests/test_dsp_stages.py.

#include "audio/audio_dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace esphome::airplay_receiver;

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
      std::exit(1);                                                                    \
    }                                                                                  \
  } while (0)

#define CHECK_NEAR(value, expected, tolerance)                                                       \
  do {                                                                                               \
    const double v_ = (value), e_ = (expected);                                                      \
    if (std::fabs(v_ - e_) > (tolerance)) {                                                          \
      std::fprintf(stderr, "%s:%d: %s = %.4f, expected %.4f +- %.4f\n", __FILE__, __LINE__, #value, \
                   v_, e_, (double) (tolerance));                                                    \
      std::exit(1);                                                                                  \
    }                                                                                                \
  } while (0)

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr size_t BLOCK_FRAMES = 352;  // one AAC-sized playout block
static constexpr size_t TONE_FRAMES = SAMPLE_RATE;  // 1 s: filters settle in the first half
static constexpr double PI = 3.14159265358979323846;
static constexpr int32_t UNITY_Q15 = 32768;
static constexpr int16_t PCM_MAX = 32767;

using Stereo = std::vector<int16_t>;

static void reset_all() {
  audio_dsp_set_sample_rate(SAMPLE_RATE);
  audio_dsp_set_filters(nullptr, 0);
  audio_dsp_set_preamp_db(0.0f);
  audio_dsp_set_loudness(100.0f, 0.0f, 30.0f);
  audio_dsp_set_bass_enhancer(90.0f, 0.0f);
  audio_dsp_set_stereo_width(1.0f, 300.0f);
  audio_dsp_set_crosstalk(0.0f, 60.0f, 250.0f, 5000.0f);
  audio_dsp_set_bass_limiter(false, 120.0f, -6.0f, 200.0f);
  audio_dsp_take_bass_limiter_reduction_db();
  audio_dsp_set_limiter(false, -1.0f, 100.0f);
  audio_dsp_set_volume_q15(UNITY_Q15);
  audio_dsp_service();
  audio_dsp_set_enabled(true);
  audio_dsp_reset();
  audio_dsp_take_limiter_reduction_db();
}

static void process(Stereo &pcm) {
  const size_t frames = pcm.size() / 2;
  for (size_t start = 0; start < frames; start += BLOCK_FRAMES) {
    const size_t count = std::min(BLOCK_FRAMES, frames - start);
    audio_dsp_process(pcm.data() + start * 2, count);
  }
}

static Stereo tone(double hz, double left_amplitude, double right_amplitude, size_t frames = TONE_FRAMES) {
  Stereo pcm(frames * 2);
  for (size_t i = 0; i < frames; i++) {
    const double s = std::sin(2.0 * PI * hz * (double) i / SAMPLE_RATE);
    pcm[i * 2] = (int16_t) std::lround(left_amplitude * s);
    pcm[i * 2 + 1] = (int16_t) std::lround(right_amplitude * s);
  }
  return pcm;
}

/// Amplitude of `hz` in one channel over the second half (Goertzel), signed by
/// phase relative to sine so an inverted channel reads negative.
static double amplitude(const Stereo &pcm, size_t channel, double hz) {
  const size_t frames = pcm.size() / 2;
  const size_t first = frames / 2;
  double re = 0.0, im = 0.0;
  for (size_t i = first; i < frames; i++) {
    const double phase = 2.0 * PI * hz * (double) i / SAMPLE_RATE;
    re += pcm[i * 2 + channel] * std::sin(phase);
    im += pcm[i * 2 + channel] * std::cos(phase);
  }
  const double n = (double) (frames - first);
  const double magnitude = 2.0 * std::sqrt(re * re + im * im) / n;
  return re >= 0.0 ? magnitude : -magnitude;
}

static double gain_db(double hz, double input_amplitude = 4000.0) {
  Stereo pcm = tone(hz, input_amplitude, input_amplitude);
  process(pcm);
  return 20.0 * std::log10(std::fabs(amplitude(pcm, 0, hz)) / input_amplitude);
}

static int16_t peak(const Stereo &pcm) {
  int peak_value = 0;
  for (int16_t s : pcm) {
    peak_value = std::max(peak_value, std::abs((int) s));
  }
  return (int16_t) std::min(peak_value, (int) PCM_MAX);
}

static void test_bypass_is_bit_exact() {
  reset_all();
  Stereo pcm = tone(1000.0, 20000.0, -12000.0, 4096);
  const Stereo original = pcm;
  process(pcm);
  CHECK(pcm == original);
  CHECK(audio_dsp_get_latency_frames() == 0);
}

static void test_limiter_holds_ceiling() {
  reset_all();
  audio_dsp_set_preamp_db(12.0f);  // drive a near-full-scale mix 12 dB into the ceiling
  audio_dsp_set_limiter(true, -1.0f, 100.0f);
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 6000.0);
  Stereo pcm(TONE_FRAMES * 2);
  for (size_t i = 0; i < TONE_FRAMES; i++) {
    const double bass = 9000.0 * std::sin(2.0 * PI * 45.0 * (double) i / SAMPLE_RATE);
    pcm[i * 2] = (int16_t) std::clamp(bass + noise(rng), -32768.0, 32767.0);
    pcm[i * 2 + 1] = (int16_t) std::clamp(-bass + noise(rng), -32768.0, 32767.0);
  }
  process(pcm);
  const int16_t ceiling = (int16_t) std::lround(std::pow(10.0, -1.0 / 20.0) * 32768.0);
  CHECK(peak(pcm) <= ceiling + 1);
  CHECK(peak(pcm) > ceiling - 600);  // it limits, it does not just turn everything down
  CHECK(audio_dsp_take_limiter_reduction_db() < -6.0f);
}

static void test_limiter_is_transparent_delay_below_ceiling() {
  reset_all();
  audio_dsp_set_limiter(true, -1.0f, 100.0f);
  const uint32_t latency = audio_dsp_get_latency_frames();
  CHECK(latency == 65);  // round(1.5 ms * 44.1 kHz) - 1
  Stereo pcm = tone(440.0, 10000.0, 5000.0, 8192);
  const Stereo original = pcm;
  process(pcm);
  for (size_t i = latency; i < original.size() / 2; i++) {
    CHECK(std::abs(pcm[i * 2] - original[(i - latency) * 2]) <= 1);
    CHECK(std::abs(pcm[i * 2 + 1] - original[(i - latency) * 2 + 1]) <= 1);
  }
  audio_dsp_set_enabled(false);
  CHECK(audio_dsp_get_latency_frames() == 0);  // bypass drops the delay too
  audio_dsp_set_enabled(true);
}

static void test_limiter_releases() {
  reset_all();
  audio_dsp_set_limiter(true, -6.0f, 100.0f);
  Stereo burst = tone(100.0, 32000.0, 32000.0, SAMPLE_RATE / 4);
  process(burst);
  CHECK(audio_dsp_take_limiter_reduction_db() < -5.0f);
  // 1 s later (10 time constants) a quiet tone must be back at full level.
  Stereo quiet = tone(1000.0, 3000.0, 3000.0);
  process(quiet);
  CHECK_NEAR(std::fabs(amplitude(quiet, 0, 1000.0)), 3000.0, 30.0);
}

static void test_loudness_follows_volume() {
  reset_all();
  audio_dsp_set_loudness(100.0f, 8.0f, 30.0f);

  audio_dsp_set_volume_q15(UNITY_Q15);
  audio_dsp_service();
  CHECK_NEAR(audio_dsp_get_loudness_boost_db(), 0.0, 0.01);
  CHECK_NEAR(gain_db(30.0), 0.0, 0.2);

  audio_dsp_set_volume_q15((int32_t) std::lround(UNITY_Q15 * std::pow(10.0, -15.0 / 20.0)));
  audio_dsp_service();
  CHECK_NEAR(audio_dsp_get_loudness_boost_db(), 4.0, 0.05);

  audio_dsp_set_volume_q15((int32_t) std::lround(UNITY_Q15 * std::pow(10.0, -40.0 / 20.0)));
  audio_dsp_service();
  CHECK_NEAR(audio_dsp_get_loudness_boost_db(), 8.0, 0.01);  // clamped past range
  CHECK_NEAR(gain_db(30.0), 8.0, 0.6);
  CHECK_NEAR(gain_db(5000.0), 0.0, 0.1);  // bass only
}

static void test_bass_enhancer_adds_linear_harmonics() {
  reset_all();
  audio_dsp_set_bass_enhancer(90.0f, 1.0f);
  const double fundamental = 50.0;

  Stereo loud = tone(fundamental, 8000.0, 8000.0);
  process(loud);
  const double second = std::fabs(amplitude(loud, 0, 2.0 * fundamental));
  const double third = std::fabs(amplitude(loud, 0, 3.0 * fundamental));
  CHECK(second > 0.25 * 8000.0);
  CHECK(third > 0.15 * 8000.0);
  CHECK(std::fabs(amplitude(loud, 1, 2.0 * fundamental) - amplitude(loud, 0, 2.0 * fundamental)) < 20.0);

  audio_dsp_reset();
  Stereo half = tone(fundamental, 4000.0, 4000.0);
  process(half);
  CHECK_NEAR(std::fabs(amplitude(half, 0, 2.0 * fundamental)) / second, 0.5, 0.05);

  // Above the corner nothing is synthesized.
  audio_dsp_reset();
  Stereo mid = tone(1000.0, 8000.0, 8000.0);
  process(mid);
  CHECK(std::fabs(amplitude(mid, 0, 2000.0)) < 8.0);
  CHECK(std::fabs(amplitude(mid, 0, 3000.0)) < 8.0);
}

static void test_stereo_width() {
  reset_all();
  audio_dsp_set_stereo_width(1.5f, 300.0f);
  // Left-only: mid = side = x/2, so side * 1.5 gives L = 1.25x, R = -0.25x.
  Stereo treble = tone(2000.0, 8000.0, 0.0);
  process(treble);
  CHECK_NEAR(amplitude(treble, 0, 2000.0), 1.25 * 8000.0, 0.03 * 8000.0);
  CHECK_NEAR(std::fabs(amplitude(treble, 1, 2000.0)), 0.25 * 8000.0, 0.03 * 8000.0);
  CHECK(amplitude(treble, 1, 2000.0) < 0.0);

  audio_dsp_reset();
  Stereo bass = tone(40.0, 8000.0, 0.0);
  process(bass);
  CHECK(std::fabs(amplitude(bass, 1, 40.0)) < 0.05 * 8000.0);
}

static void test_crosstalk_cancellation() {
  reset_all();
  const double amount = 0.7;
  audio_dsp_set_crosstalk((float) amount, 60.0f, 250.0f, 5000.0f);

  // In band the recursion lifts side toward 1 / (1 - amount) and drops mid
  // toward 1 / (1 + amount); band filter phase and the delay keep it short of
  // both limits.
  Stereo side = tone(1000.0, 4000.0, -4000.0);
  process(side);
  const double side_db = 20.0 * std::log10(std::fabs(amplitude(side, 0, 1000.0)) / 4000.0);
  CHECK(side_db > 6.0);
  CHECK(side_db < 20.0 * std::log10(1.0 / (1.0 - amount)) + 0.1);
  CHECK_NEAR(amplitude(side, 0, 1000.0), -amplitude(side, 1, 1000.0), 10.0);

  audio_dsp_reset();
  Stereo mid = tone(1000.0, 4000.0, 4000.0);
  process(mid);
  const double mid_db = 20.0 * std::log10(std::fabs(amplitude(mid, 0, 1000.0)) / 4000.0);
  CHECK(mid_db < -2.0);
  CHECK(mid_db > 20.0 * std::log10(1.0 / (1.0 + amount)) - 0.1);

  // Bass is outside the band and passes nearly untouched.
  audio_dsp_reset();
  Stereo bass = tone(50.0, 4000.0, -4000.0);
  process(bass);
  CHECK_NEAR(20.0 * std::log10(std::fabs(amplitude(bass, 0, 50.0)) / 4000.0), 0.0, 0.5);

  // Impulse response decays: the recursion is stable at the maximum amount.
  audio_dsp_set_crosstalk(0.95f, 300.0f, 250.0f, 5000.0f);
  audio_dsp_reset();
  Stereo impulse(SAMPLE_RATE * 2, 0);
  impulse[0] = 30000;
  process(impulse);
  for (size_t i = impulse.size() / 2; i < impulse.size(); i++) {
    CHECK(impulse[i] == 0);
  }
}

static void test_bass_limiter() {
  reset_all();
  audio_dsp_set_bass_limiter(true, 120.0f, -12.0f, 200.0f);
  CHECK(audio_dsp_get_latency_frames() == 65);

  // Below threshold the split and recombine is flat (Linkwitz-Riley sums to an
  // allpass), bass and treble alike.
  CHECK_NEAR(gain_db(60.0, 2000.0), 0.0, 0.1);
  audio_dsp_reset();
  CHECK_NEAR(gain_db(120.0, 2000.0), 0.0, 0.1);
  audio_dsp_reset();
  CHECK_NEAR(gain_db(3000.0, 2000.0), 0.0, 0.1);

  // A loud bass note under a loud treble tone: the bass is held near the
  // ceiling while the treble keeps its level. A full-band limiter would have
  // pulled both down together.
  audio_dsp_reset();
  const double ceiling = std::pow(10.0, -12.0 / 20.0) * 32768.0;
  Stereo mix(TONE_FRAMES * 2);
  for (size_t i = 0; i < TONE_FRAMES; i++) {
    const double t = (double) i / SAMPLE_RATE;
    const double v = 20000.0 * std::sin(2.0 * PI * 50.0 * t) + 8000.0 * std::sin(2.0 * PI * 2000.0 * t);
    mix[i * 2] = mix[i * 2 + 1] = (int16_t) std::lround(v);
  }
  process(mix);
  // The ceiling applies to the low band. The high band still carries the 50 Hz
  // note at the LR4 high-pass's (50/120)^4 -- about 600 here -- unlimited.
  const double high_band_leak = 20000.0 * std::pow(50.0 / 120.0, 4.0);
  CHECK_NEAR(std::fabs(amplitude(mix, 0, 50.0)), ceiling + high_band_leak, 0.03 * ceiling);
  CHECK_NEAR(std::fabs(amplitude(mix, 0, 2000.0)), 8000.0, 80.0);
  CHECK(audio_dsp_take_bass_limiter_reduction_db() < -3.0f);

  // Both limiters: the delays add.
  audio_dsp_set_limiter(true, -1.0f, 100.0f);
  CHECK(audio_dsp_get_latency_frames() == 130);
}

static void test_sample_rate_change_rescales_lookahead() {
  reset_all();
  audio_dsp_set_limiter(true, -1.0f, 100.0f);
  audio_dsp_set_sample_rate(48000);
  CHECK(audio_dsp_get_latency_frames() == 71);  // round(1.5 ms * 48 kHz) - 1
  Stereo pcm = tone(440.0, 30000.0, 30000.0, 4096);
  process(pcm);  // state resized on the audio side without tripping anything
  audio_dsp_set_sample_rate(SAMPLE_RATE);
}

int main() {
  test_bypass_is_bit_exact();
  test_limiter_holds_ceiling();
  test_limiter_is_transparent_delay_below_ceiling();
  test_limiter_releases();
  test_loudness_follows_volume();
  test_bass_enhancer_adds_linear_harmonics();
  test_stereo_width();
  test_crosstalk_cancellation();
  test_bass_limiter();
  test_sample_rate_change_rescales_lookahead();
  std::printf("ok\n");
  return 0;
}
