#include "audio/audio_timeline.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace esphome::airplay_receiver;

extern "C" void *airplay_alloc(size_t bytes, bool) { return std::malloc(bytes); }
extern "C" void *airplay_calloc(size_t count, size_t bytes, bool) {
  return std::calloc(count, bytes);
}
extern "C" void airplay_free(void *memory) { std::free(memory); }

static constexpr uint32_t FRAME = 352;
static constexpr uint32_t EPOCH = 7;
static constexpr double SAMPLE_RATE = 44100.0;
static constexpr double PI = 3.14159265358979;
static constexpr double SINE_AMPLITUDE = 12000.0;
/* A dropout this far below the signal's RMS counts as silence. */
static constexpr double MIN_FILL_RMS_RATIO = 0.6;

static void insert(audio_timeline_t &timeline, uint32_t rtp,
                   int16_t left = 24000, int16_t right = -24000) {
  audio_timeline_reservation_t reservation{};
  int16_t *pcm = nullptr;
  assert(audio_timeline_reserve(&timeline, EPOCH, rtp, &reservation, &pcm));
  assert(!reservation.duplicate);
  for (uint32_t i = 0; i < timeline.frame_samples; i++) {
    pcm[i * 2] = left;
    pcm[i * 2 + 1] = right;
  }
  assert(audio_timeline_commit(&timeline, &reservation));
}

static double sine(uint32_t frame, double hz) {
  return SINE_AMPLITUDE * std::sin(2.0 * PI * hz * frame / SAMPLE_RATE);
}

/* One block of a stereo sine whose phase follows the absolute frame index. */
static void insert_sine(audio_timeline_t &timeline, uint32_t origin,
                        uint32_t block, double hz) {
  audio_timeline_reservation_t reservation{};
  int16_t *pcm = nullptr;
  const uint32_t rtp = origin + block * FRAME;
  assert(audio_timeline_reserve(&timeline, EPOCH, rtp, &reservation, &pcm));
  for (uint32_t i = 0; i < FRAME; i++) {
    const auto v = static_cast<int16_t>(std::lround(sine(block * FRAME + i, hz)));
    pcm[i * 2] = v;
    pcm[i * 2 + 1] = static_cast<int16_t>(-v);
  }
  assert(audio_timeline_commit(&timeline, &reservation));
}

static std::vector<int16_t> read(audio_timeline_t &timeline, uint32_t rtp,
                                size_t frames, size_t expected_missing) {
  std::vector<int16_t> pcm(frames * 2);
  size_t concealed = SIZE_MAX;
  assert(audio_timeline_read(&timeline, EPOCH, rtp, pcm.data(), frames, 2,
                             true, &concealed) == frames);
  assert(concealed == expected_missing);
  return pcm;
}

static int max_step(const std::vector<int16_t> &pcm) {
  int step = 0;
  for (size_t i = 2; i < pcm.size(); i++) {
    step = std::max(step, std::abs(int(pcm[i]) - pcm[i - 2]));
  }
  return step;
}

static double rms(const std::vector<int16_t> &pcm, size_t first_frame,
                  size_t frames) {
  double sum = 0.0;
  for (size_t i = first_frame; i < first_frame + frames; i++) {
    sum += double(pcm[i * 2]) * pcm[i * 2];
  }
  return std::sqrt(sum / frames);
}

/* A full-scale step across the hole, with too little audio on either side for
 * a period search: the fill must ramp between the two levels without a jump,
 * across split reads, and the RTP accounting must not change. */
static void check_gap(uint32_t origin, uint32_t missing_frames) {
  audio_timeline_t timeline{};
  assert(audio_timeline_init(&timeline, 8, FRAME) == ESP_OK);
  insert(timeline, origin, INT16_MAX, INT16_MIN);
  insert(timeline, origin + FRAME + missing_frames, INT16_MIN, INT16_MAX);
  auto out = read(timeline, origin, FRAME, 0);
  assert(out[0] == INT16_MAX && out.back() == INT16_MIN);

  uint32_t offset = 0;
  for (size_t quantum : {size_t{1}, size_t{23}, size_t{131}, size_t{4096}}) {
    const size_t count = std::min(quantum, size_t(missing_frames - offset));
    if (count == 0) {
      continue;
    }
    auto part = read(timeline, origin + FRAME + offset, count, count);
    out.insert(out.end(), part.begin(), part.end());
    offset += count;
  }
  assert(offset == missing_frames);

  const uint32_t resume = origin + FRAME + missing_frames;
  auto first = read(timeline, resume, 1, 0);
  auto rest = read(timeline, resume + 1, FRAME - 1, 0);
  out.insert(out.end(), first.begin(), first.end());
  out.insert(out.end(), rest.begin(), rest.end());
  const int ramp_bound = int(65535.0 * PI / (2.0 * missing_frames)) + 64;
  assert(max_step(out) <= ramp_bound);
  assert(rest[(PLC_SEAM_FRAMES - 2) * 2] == INT16_MIN);
  assert(rest.back() == INT16_MAX);

  // A clear with the same epoch and a contiguous RTP must still erase history.
  audio_timeline_clear(&timeline);
  insert(timeline, resume + FRAME, 12345, -12345);
  auto fresh = read(timeline, resume + FRAME, FRAME, 0);
  assert(fresh.front() == 12345 && fresh.back() == -12345);
  audio_timeline_deinit(&timeline);
}

/* A 17-packet hole (136 ms) in a sine, the measured burst size: the fill must
 * stay near the true signal and keep its level, not drop to silence. */
static void check_two_sided_fill() {
  static constexpr uint32_t BLOCKS = 48, HOLE_FIRST = 16, HOLE_BLOCKS = 17;
  static constexpr double HZ = 440.0;
  static constexpr uint32_t ORIGIN = 5000;
  audio_timeline_t timeline{};
  assert(audio_timeline_init(&timeline, 64, FRAME) == ESP_OK);
  for (uint32_t b = 0; b < BLOCKS; b++) {
    if (b < HOLE_FIRST || b >= HOLE_FIRST + HOLE_BLOCKS) {
      insert_sine(timeline, ORIGIN, b, HZ);
    }
  }
  std::vector<int16_t> out;
  for (uint32_t b = 0; b < BLOCKS; b++) {
    const bool missing = b >= HOLE_FIRST && b < HOLE_FIRST + HOLE_BLOCKS;
    auto part = read(timeline, ORIGIN + b * FRAME, FRAME, missing ? FRAME : 0);
    out.insert(out.end(), part.begin(), part.end());
  }
  const double slope = SINE_AMPLITUDE * 2.0 * PI * HZ / SAMPLE_RATE;
  assert(max_step(out) <= int(2.5 * slope));
  const size_t hole_start = HOLE_FIRST * FRAME, hole_frames = HOLE_BLOCKS * FRAME;
  assert(rms(out, hole_start, hole_frames) >=
         MIN_FILL_RMS_RATIO * SINE_AMPLITUDE / std::sqrt(2.0));
  for (size_t i = hole_start; i < hole_start + hole_frames; i++) {
    assert(std::abs(out[i * 2] - sine(uint32_t(i), HZ)) <= 0.25 * SINE_AMPLITUDE);
    assert(out[i * 2 + 1] == -out[i * 2] || out[i * 2 + 1] == -out[i * 2] - 1 ||
           out[i * 2 + 1] == -out[i * 2] + 1);
  }
  assert(timeline.plc.two_sided_events == 1 && timeline.plc.forward_events == 0);
  audio_timeline_deinit(&timeline);
}

/* No audio after the hole yet: hold the extension, fade out, then silence,
 * and fade in from silence when audio returns. */
static void check_forward_only() {
  static constexpr uint32_t ORIGIN = 9000, PLAYED = 12, HOLE_BLOCKS = 30;
  static constexpr double HZ = 300.0;
  audio_timeline_t timeline{};
  assert(audio_timeline_init(&timeline, 64, FRAME) == ESP_OK);
  for (uint32_t b = 0; b < PLAYED; b++) {
    insert_sine(timeline, ORIGIN, b, HZ);
  }
  std::vector<int16_t> out;
  for (uint32_t b = 0; b < PLAYED + HOLE_BLOCKS; b++) {
    auto part = read(timeline, ORIGIN + b * FRAME, FRAME, b < PLAYED ? 0 : FRAME);
    out.insert(out.end(), part.begin(), part.end());
  }
  const size_t hole_start = PLAYED * FRAME;
  assert(rms(out, hole_start, PLC_HOLD_FRAMES) >=
         MIN_FILL_RMS_RATIO * SINE_AMPLITUDE / std::sqrt(2.0));
  const size_t silent_from = hole_start + PLC_HOLD_FRAMES + PLC_FADE_FRAMES;
  for (size_t i = silent_from * 2; i < out.size(); i++) {
    assert(out[i] == 0);
  }
  const double slope = SINE_AMPLITUDE * 2.0 * PI * HZ / SAMPLE_RATE;
  assert(max_step(out) <= int(2.5 * slope));

  const uint32_t resume_block = PLAYED + HOLE_BLOCKS;
  insert_sine(timeline, ORIGIN, resume_block, HZ);
  auto resumed = read(timeline, ORIGIN + resume_block * FRAME, FRAME, 0);
  assert(std::abs(int(resumed[0])) <= int(SINE_AMPLITUDE / PLC_SEAM_FRAMES) + 1);
  assert(timeline.plc.forward_events == 1 && timeline.plc.two_sided_events == 0);
  audio_timeline_deinit(&timeline);
}

static void check_period_search() {
  static constexpr uint32_t FRAMES = 3000, PERIOD = 100;  // 441 Hz
  std::vector<int16_t> pcm(FRAMES * PLC_STRIDE);
  std::vector<float> scratch(FRAMES / PLC_DECIMATION * PLC_STRIDE);
  for (uint32_t i = 0; i < FRAMES; i++) {
    const auto v = static_cast<int16_t>(
        std::lround(SINE_AMPLITUDE * std::sin(2.0 * PI * i / PERIOD)));
    pcm[i * 2] = pcm[i * 2 + 1] = v;
  }
  for (bool at_end : {true, false}) {
    const uint32_t period =
        audio_plc_find_period(pcm.data(), FRAMES, at_end, scratch.data());
    assert(period >= PLC_MIN_PERIOD && period <= PLC_MAX_PERIOD);
    assert(period % PERIOD == 0);
  }
  std::fill(pcm.begin(), pcm.end(), 0);
  assert(audio_plc_find_period(pcm.data(), FRAMES, true, scratch.data()) ==
         PLC_MIN_PERIOD);
  assert(audio_plc_find_period(pcm.data(), PLC_MIN_PERIOD, true,
                               scratch.data()) == 0);
}

static void check_healthy_audio_and_aac() {
  audio_timeline_t timeline{};
  assert(audio_timeline_init(&timeline, 8, 1024) == ESP_OK);
  insert(timeline, 1000, 17000, -12000);
  insert(timeline, 2024, 17000, -12000);
  for (uint32_t position = 1000; position < 3048; position += 1) {
    auto pcm = read(timeline, position, 1, 0);
    assert(pcm[0] == 17000 && pcm[1] == -12000);
  }
  audio_timeline_deinit(&timeline);
}

int main() {
  check_gap(1000, FRAME);
  check_gap(UINT32_MAX - 500, FRAME * 3);
  check_two_sided_fill();
  check_forward_only();
  check_period_search();
  check_healthy_audio_and_aac();
  std::puts("ok");
}
