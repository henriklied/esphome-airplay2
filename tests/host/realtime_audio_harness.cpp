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

static std::vector<int16_t> read(audio_timeline_t &timeline, uint32_t rtp,
                                size_t frames, size_t expected_missing) {
  std::vector<int16_t> pcm(frames * 2);
  size_t concealed = SIZE_MAX;
  assert(audio_timeline_read(&timeline, EPOCH, rtp, pcm.data(), frames, 2,
                             true, &concealed) == frames);
  assert(concealed == expected_missing);
  return pcm;
}

static void check_gap(uint32_t origin, uint32_t missing_frames) {
  audio_timeline_t timeline{};
  assert(audio_timeline_init(&timeline, 8, FRAME) == ESP_OK);
  insert(timeline, origin, INT16_MAX, INT16_MIN);
  insert(timeline, origin + FRAME + missing_frames, INT16_MIN, INT16_MAX);
  auto healthy = read(timeline, origin, FRAME, 0);
  assert(healthy[0] == INT16_MAX && healthy.back() == INT16_MIN);

  // Split each gap across arbitrary render sizes, including a servo read.
  std::vector<int16_t> gap;
  uint32_t offset = 0;
  for (size_t quantum : {size_t{1}, size_t{23}, size_t{131}, size_t{4096}}) {
    const size_t count = std::min(quantum, size_t(missing_frames - offset));
    if (count == 0) {
      continue;
    }
    auto part = read(timeline, origin + FRAME + offset, count, count);
    gap.insert(gap.end(), part.begin(), part.end());
    offset += count;
  }
  assert(offset == missing_frames);
  assert(gap[0] == INT16_MAX && gap[1] == INT16_MIN);
  for (size_t i = 2; i < gap.size(); i += 2) {
    assert(gap[i] <= gap[i - 2] && gap[i] >= 0);
    assert(gap[i + 1] >= gap[i - 1] && gap[i + 1] <= 0);
    assert(std::abs(int(gap[i]) - gap[i - 2]) <= 373);
  }
  assert(gap[88 * 2] == 0 && gap.back() == 0);

  const uint32_t resume = origin + FRAME + missing_frames;
  auto first = read(timeline, resume, 1, 0);
  auto rest = read(timeline, resume + 1, FRAME - 1, 0);
  assert(std::abs(int(first[0])) <= 373);
  assert(first[1] >= 0 && first[1] <= 373);
  assert(rest[(88 - 2) * 2] == INT16_MIN);
  assert(rest.back() == INT16_MAX);

  // A clear with the same epoch and a contiguous RTP must still erase history.
  audio_timeline_clear(&timeline);
  insert(timeline, resume + FRAME, 12345, -12345);
  auto fresh = read(timeline, resume + FRAME, FRAME, 0);
  assert(fresh.front() == 12345 && fresh.back() == -12345);
  audio_timeline_deinit(&timeline);
}

static void check_short_gap_and_reset() {
  audio_gap_fader_t fader{};
  fader.reset();
  int16_t last[] = {20000, -20000};
  fader.recover(last, 1, 2);
  int16_t gap[6];
  fader.conceal(gap, 3, 2);
  int16_t resume[] = {-20000, 20000};
  fader.recover(resume, 1, 2);
  assert(std::abs(int(resume[0]) - gap[4]) < 500);
  assert(std::abs(int(resume[1]) - gap[5]) < 500);
  fader.reset();
  fader.conceal(gap, 3, 2);
  assert(std::all_of(std::begin(gap), std::end(gap), [](int16_t x) { return x == 0; }));
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
  check_short_gap_and_reset();
  check_healthy_audio_and_aac();
  std::puts("ok");
}
