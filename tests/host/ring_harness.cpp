// Host checks for audio/buffered_ring.h. Exits non-zero with a message on the
// first failure. Driven by tests/test_buffered_ring.py.

#include "audio/buffered_ring.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

using esphome::airplay_receiver::BUFFERED_FRAME_MIN_BYTES;
using esphome::airplay_receiver::BUFFERED_FRAME_PREFIX_BYTES;
using esphome::airplay_receiver::BufferedFrameStatus;
using esphome::airplay_receiver::buffered_ring_pop_frame;
using esphome::airplay_receiver::ByteRing;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                   __LINE__, #cond);                                   \
      std::exit(1);                                                    \
    }                                                                  \
  } while (0)

static constexpr size_t SMALL_CAPACITY = 97;  // prime, so wraps land everywhere
static constexpr size_t OUT_CAPACITY = 64;
static constexpr int FUZZ_ROUNDS = 200000;

static std::vector<uint8_t> make_frame(size_t body_len, uint8_t seed) {
  const size_t declared = body_len + BUFFERED_FRAME_PREFIX_BYTES;
  std::vector<uint8_t> frame{(uint8_t) (declared >> 8), (uint8_t) declared};
  for (size_t i = 0; i < body_len; i++) {
    frame.push_back((uint8_t) (seed + i));
  }
  return frame;
}

// Write as much of `bytes` as fits, in at most two region writes; returns count.
static size_t write_some(ByteRing *ring, const uint8_t *bytes, size_t len) {
  size_t written = 0;
  while (written < len) {
    size_t space = 0;
    uint8_t *region = ring->write_region(&space);
    if (!region) {
      break;
    }
    const size_t n = space < len - written ? space : len - written;
    std::copy(bytes + written, bytes + written + n, region);
    ring->commit(n);
    written += n;
  }
  return written;
}

static void test_empty_and_full() {
  std::vector<uint8_t> storage(SMALL_CAPACITY);
  ByteRing ring;
  ring.attach(storage.data(), storage.size());
  uint8_t out[OUT_CAPACITY];
  size_t len = 0;
  CHECK(buffered_ring_pop_frame(&ring, out, sizeof(out), &len) == BufferedFrameStatus::INCOMPLETE);

  std::vector<uint8_t> filler(SMALL_CAPACITY, 0xAB);
  CHECK(write_some(&ring, filler.data(), filler.size()) == SMALL_CAPACITY);
  size_t space = 1;
  CHECK(ring.write_region(&space) == nullptr && space == 0);
  CHECK(ring.free_space() == 0);
}

static void test_partial_frame_waits() {
  std::vector<uint8_t> storage(SMALL_CAPACITY);
  ByteRing ring;
  ring.attach(storage.data(), storage.size());
  const auto frame = make_frame(20, 7);
  uint8_t out[OUT_CAPACITY];
  size_t len = 0;

  CHECK(write_some(&ring, frame.data(), 1) == 1);
  CHECK(buffered_ring_pop_frame(&ring, out, sizeof(out), &len) == BufferedFrameStatus::INCOMPLETE);
  CHECK(write_some(&ring, frame.data() + 1, frame.size() - 2) == frame.size() - 2);
  CHECK(buffered_ring_pop_frame(&ring, out, sizeof(out), &len) == BufferedFrameStatus::INCOMPLETE);
  CHECK(ring.size() == frame.size() - 1);  // nothing consumed while incomplete
  CHECK(write_some(&ring, frame.data() + frame.size() - 1, 1) == 1);
  CHECK(buffered_ring_pop_frame(&ring, out, sizeof(out), &len) == BufferedFrameStatus::READY);
  CHECK(len == 20 && out[0] == 7 && out[19] == 26 && ring.size() == 0);
}

static void test_invalid_prefix() {
  std::vector<uint8_t> storage(SMALL_CAPACITY);
  ByteRing ring;
  ring.attach(storage.data(), storage.size());
  uint8_t out[OUT_CAPACITY];
  size_t len = 0;

  const uint8_t too_short[] = {0, (uint8_t) (BUFFERED_FRAME_MIN_BYTES - 1)};
  write_some(&ring, too_short, sizeof(too_short));
  CHECK(buffered_ring_pop_frame(&ring, out, sizeof(out), &len) == BufferedFrameStatus::INVALID);
  CHECK(len == BUFFERED_FRAME_MIN_BYTES - 1);

  ring.clear();
  const size_t too_long = OUT_CAPACITY + BUFFERED_FRAME_PREFIX_BYTES + 1;
  const uint8_t oversized[] = {(uint8_t) (too_long >> 8), (uint8_t) too_long};
  write_some(&ring, oversized, sizeof(oversized));
  CHECK(buffered_ring_pop_frame(&ring, out, sizeof(out), &len) == BufferedFrameStatus::INVALID);
  CHECK(len == too_long);
}

// Random writes and pops against a byte-for-byte model of the stream.
static void test_fuzz_against_model() {
  std::vector<uint8_t> storage(SMALL_CAPACITY);
  ByteRing ring;
  ring.attach(storage.data(), storage.size());
  std::mt19937 rng(12345);
  std::deque<uint8_t> pending;   // generated but not yet written
  std::deque<std::vector<uint8_t>> expected;  // frame bodies in order
  uint8_t seed = 0;
  size_t frames_popped = 0;

  for (int round = 0; round < FUZZ_ROUNDS; round++) {
    if (pending.size() < OUT_CAPACITY * 2) {
      const size_t body = 12 + rng() % (OUT_CAPACITY - 12 + 1);
      const auto frame = make_frame(body, seed++);
      pending.insert(pending.end(), frame.begin(), frame.end());
      expected.emplace_back(frame.begin() + BUFFERED_FRAME_PREFIX_BYTES, frame.end());
    }
    if (rng() % 2) {
      const size_t want = 1 + rng() % 40;
      std::vector<uint8_t> chunk(pending.begin(),
                                 pending.begin() + (want < pending.size() ? want : pending.size()));
      const size_t written = write_some(&ring, chunk.data(), chunk.size());
      pending.erase(pending.begin(), pending.begin() + written);
    } else {
      uint8_t out[OUT_CAPACITY];
      size_t len = 0;
      const auto status = buffered_ring_pop_frame(&ring, out, sizeof(out), &len);
      CHECK(status != BufferedFrameStatus::INVALID);
      if (status == BufferedFrameStatus::READY) {
        CHECK(!expected.empty());
        CHECK(len == expected.front().size());
        CHECK(std::equal(out, out + len, expected.front().begin()));
        expected.pop_front();
        frames_popped++;
      }
    }
    CHECK(ring.size() + ring.free_space() == SMALL_CAPACITY);
  }
  CHECK(frames_popped > 1000);
}

int main() {
  test_empty_and_full();
  test_partial_frame_waits();
  test_invalid_prefix();
  test_fuzz_against_model();
  std::puts("ok");
  return 0;
}
