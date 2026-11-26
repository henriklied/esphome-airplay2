// Host checks for the stereo-pair link: wire format (audio/peer_protocol.h)
// and the relayed clock filter (audio/peer_clock.h).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "audio/peer_clock.h"
#include "audio/peer_protocol.h"

using namespace esphome::airplay_receiver::peer;

static int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static void test_pcm_round_trip() {
  uint8_t datagram[MAX_DATAGRAM];
  const uint8_t channels = 1;
  const size_t capacity = pcm_chunk_capacity(channels);
  CHECK(capacity == 640);

  int16_t samples[640];
  for (size_t i = 0; i < capacity; i++) {
    samples[i] = (int16_t) (i * 97 - 30000);
  }
  PcmChunk chunk{};
  chunk.epoch = 7;
  chunk.rtp = 0xFFFFFF00u;
  chunk.flush_serial = 3;
  chunk.flush_rtp = 123456;
  chunk.frame_samples = 1024;
  chunk.offset = 384;
  chunk.count = 640;
  chunk.channels = channels;
  const size_t length = encode_pcm(datagram, sizeof(datagram), 0xABCDEF01u, 42, chunk, samples);
  CHECK(length == HEADER_BYTES + PCM_FIELDS_BYTES + 640 * 2);
  CHECK(length <= MAX_DATAGRAM);

  Reader r(datagram, length);
  Header header{};
  CHECK(read_header(r, &header));
  CHECK(header.type == (uint8_t) MsgType::PCM);
  CHECK(header.boot_id == 0xABCDEF01u);
  CHECK(header.seq == 42);
  PcmChunk decoded{};
  int16_t out[640];
  CHECK(decode_pcm(r, &decoded, out, 640));
  CHECK(decoded.epoch == 7 && decoded.rtp == 0xFFFFFF00u && decoded.flush_serial == 3 &&
        decoded.flush_rtp == 123456 && decoded.frame_samples == 1024 && decoded.offset == 384 &&
        decoded.count == 640 && decoded.channels == 1);
  for (size_t i = 0; i < capacity; i++) {
    CHECK(out[i] == samples[i]);
  }

  // A stereo chunk at capacity also fits one datagram.
  CHECK(pcm_chunk_capacity(2) == 320);
  int16_t stereo[640] = {};
  chunk.channels = 2;
  chunk.count = 320;
  chunk.offset = 0;
  CHECK(encode_pcm(datagram, sizeof(datagram), 1, 1, chunk, stereo) <= MAX_DATAGRAM);
}

static void test_pcm_rejects_corruption() {
  uint8_t datagram[MAX_DATAGRAM];
  int16_t samples[640] = {};
  PcmChunk chunk{};
  chunk.frame_samples = 352;
  chunk.offset = 0;
  chunk.count = 352;
  chunk.channels = 1;
  const size_t length = encode_pcm(datagram, sizeof(datagram), 1, 1, chunk, samples);
  CHECK(length > 0);

  // Truncated payload.
  {
    Reader r(datagram, length - 2);
    Header header{};
    CHECK(read_header(r, &header));
    PcmChunk decoded{};
    int16_t out[640];
    CHECK(!decode_pcm(r, &decoded, out, 640));
  }
  // A chunk that would run past the end of its frame.
  {
    chunk.offset = 100;
    const size_t bad = encode_pcm(datagram, sizeof(datagram), 1, 1, chunk, samples);
    Reader r(datagram, bad);
    Header header{};
    CHECK(read_header(r, &header));
    PcmChunk decoded{};
    int16_t out[640];
    CHECK(!decode_pcm(r, &decoded, out, 640));
  }
  // Wrong magic and a newer version are not ours.
  {
    datagram[0] ^= 0xFF;
    Reader r(datagram, length);
    Header header{};
    CHECK(!read_header(r, &header));
    datagram[0] ^= 0xFF;
    datagram[4] = VERSION + 1;
    Reader r2(datagram, length);
    CHECK(!read_header(r2, &header));
  }
  // Output buffer too small for what the chunk claims.
  {
    chunk.offset = 0;
    const size_t ok = encode_pcm(datagram, sizeof(datagram), 1, 1, chunk, samples);
    Reader r(datagram, ok);
    Header header{};
    CHECK(read_header(r, &header));
    PcmChunk decoded{};
    int16_t out[640];
    CHECK(!decode_pcm(r, &decoded, out, 100));
  }
}

static void test_state_round_trip() {
  State s{};
  s.epoch = 9;
  s.flush_serial = 2;
  s.flush_rtp = 77;
  s.sample_rate = 44100;
  s.frame_samples = 1024;
  s.engine_ready = true;
  s.playing = true;
  s.output_running = false;
  s.anchor_valid = true;
  s.domain = AnchorDomain::LOCAL;
  s.anchor_rtp = 0x80000001u;
  s.anchor_network_ns = 0x123456789ABCDEFULL;
  s.playout_offset_ns = -250000000LL;
  s.clock_id = 0xBAC6D397D0D50008ULL;
  s.volume_q15 = 16384;
  s.forwarding = true;
  s.session = 0xC0FFEE;
  uint8_t datagram[MAX_DATAGRAM];
  const size_t length = encode_state(datagram, sizeof(datagram), 5, 6, s);
  CHECK(length == HEADER_BYTES + STATE_FIELDS_BYTES);
  Reader r(datagram, length);
  Header header{};
  CHECK(read_header(r, &header));
  CHECK(header.type == (uint8_t) MsgType::STATE);
  State d{};
  CHECK(decode_state(r, &d));
  CHECK(d.epoch == 9 && d.flush_serial == 2 && d.flush_rtp == 77 && d.sample_rate == 44100 &&
        d.frame_samples == 1024 && d.engine_ready && d.playing && !d.output_running && d.anchor_valid &&
        d.domain == AnchorDomain::LOCAL && d.anchor_rtp == 0x80000001u &&
        d.anchor_network_ns == 0x123456789ABCDEFULL && d.playout_offset_ns == -250000000LL &&
        d.clock_id == 0xBAC6D397D0D50008ULL && d.volume_q15 == 16384 && d.forwarding && d.session == 0xC0FFEE);

  // An unknown domain is rejected rather than misread.
  datagram[HEADER_BYTES + 21] = 9;
  Reader bad(datagram, length);
  CHECK(read_header(bad, &header));
  CHECK(!decode_state(bad, &d));
}

static void test_ping_pong_round_trip() {
  uint8_t datagram[MAX_DATAGRAM];
  Ping ping{-5};
  size_t length = encode_ping(datagram, sizeof(datagram), 1, 2, ping);
  CHECK(length == HEADER_BYTES + PING_FIELDS_BYTES);
  Reader r(datagram, length);
  Header header{};
  CHECK(read_header(r, &header));
  Ping decoded_ping{};
  CHECK(decode_ping(r, &decoded_ping) && decoded_ping.t1_local_ns == -5);

  Pong pong{1, 2, 3, -4, true};
  length = encode_pong(datagram, sizeof(datagram), 1, 2, pong);
  CHECK(length == HEADER_BYTES + PONG_FIELDS_BYTES);
  Reader r2(datagram, length);
  CHECK(read_header(r2, &header));
  Pong d{};
  CHECK(decode_pong(r2, &d));
  CHECK(d.t1_local_ns == 1 && d.t2_primary_ns == 2 && d.t3_primary_ns == 3 && d.network_offset_ns == -4 &&
        d.network_valid);
}

static void test_serial_wrap() {
  CHECK(serial_newer(1, 0));
  CHECK(!serial_newer(0, 1));
  CHECK(serial_newer(2, 0xFFFFFFFEu));
  CHECK(!serial_newer(5, 5));
}

// Simulated link: the secondary's crystal runs `local_ppm` off true time, the
// primary's network clock `network_ppm` off; one-way delays are a floor plus
// exponential queueing, independent per direction, with occasional stalls.
struct Link {
  double local_ppm;
  double network_ppm;
  int64_t local_origin_ns;
  int64_t network_origin_ns;
  double queueing_mean_ns;
  double stall_probability;
  std::mt19937_64 rng;

  int64_t local(int64_t true_ns) const {
    return local_origin_ns + true_ns + (int64_t) (true_ns * local_ppm * 1e-6);
  }
  int64_t network(int64_t true_ns) const {
    return network_origin_ns + true_ns + (int64_t) (true_ns * network_ppm * 1e-6);
  }
  int64_t delay() {
    std::exponential_distribution<double> queueing(1.0 / queueing_mean_ns);
    std::uniform_real_distribution<double> stall(0.0, 1.0);
    int64_t d = 1500000 + (int64_t) queueing(rng);
    if (stall(rng) < stall_probability) {
      d += 40000000;
    }
    return d;
  }
};

struct ClockError {
  double worst_ns;
  double rms_ns;
};

static constexpr int64_t EXCHANGE_INTERVAL_NS = 100000000;  // 10 exchanges/s

static ClockError run_clock(Link link, int64_t settle_ns, int64_t measure_ns, PeerClock *clock,
                            int64_t step_at_ns = -1, int64_t step_ns = 0) {
  ClockError result{0.0, 0.0};
  double sum_squares = 0.0;
  int probes = 0;
  for (int64_t t = 0; t < settle_ns + measure_ns; t += EXCHANGE_INTERVAL_NS) {
    if (step_at_ns >= 0 && t >= step_at_ns && t < step_at_ns + EXCHANGE_INTERVAL_NS) {
      link.network_origin_ns += step_ns;
    }
    const int64_t up = link.delay();
    const int64_t down = link.delay();
    const int64_t t1 = link.local(t);
    const int64_t t2 = link.network(t + up);
    const int64_t t3 = link.network(t + up + 50000);
    const int64_t t4 = link.local(t + up + 50000 + down);
    clock->add_sample(t1, t2, t3, t4);

    if (t >= settle_ns) {
      // Check the estimate halfway to the next exchange.
      const int64_t probe = t + EXCHANGE_INTERVAL_NS / 2;
      int64_t offset = 0;
      if (!clock->offset_at(link.local(probe), &offset)) {
        return ClockError{1e18, 1e18};
      }
      const double error = std::fabs((double) (link.local(probe) + offset - link.network(probe)));
      if (error > result.worst_ns) {
        result.worst_ns = error;
      }
      sum_squares += error * error;
      probes++;
    }
  }
  result.rms_ns = std::sqrt(sum_squares / probes);
  return result;
}

// Wi-Fi on a busy channel: 2 ms mean queueing each way and a 40 ms stall on
// one exchange in twenty.
static constexpr double BUSY_QUEUEING_NS = 2.0e6;
static constexpr double BUSY_STALLS = 0.05;

static void test_clock_tracks_drift() {
  PeerClock clock;
  Link link{+23.0, -11.0, 5000000000000LL, 1700000000000000000LL, BUSY_QUEUEING_NS, BUSY_STALLS,
            std::mt19937_64(1)};
  const ClockError error = run_clock(link, 20000000000LL, 300000000000LL, &clock);
  std::printf("busy link: worst %.1f us, rms %.1f us, min_rtt %.2f ms, rate %.2f ppm\n", error.worst_ns / 1e3,
              error.rms_ns / 1e3, (double) clock.min_rtt_ns() / 1e6, clock.rate() * 1e6);
  CHECK(error.worst_ns < 350000.0);
  CHECK(error.rms_ns < 100000.0);
  CHECK(std::fabs(clock.rate() * 1e6 - (-34.0)) < 5.0);
}

static void test_clock_calm_link() {
  PeerClock clock;
  Link link{-17.0, +4.0, 0, 1700000000000000000LL, 0.3e6, 0.0, std::mt19937_64(3)};
  const ClockError error = run_clock(link, 20000000000LL, 300000000000LL, &clock);
  std::printf("calm link: worst %.1f us, rms %.1f us\n", error.worst_ns / 1e3, error.rms_ns / 1e3);
  CHECK(error.worst_ns < 60000.0);
  CHECK(error.rms_ns < 20000.0);
}

static void test_clock_reseeds_after_step() {
  PeerClock clock;
  Link link{+5.0, 0.0, 0, 1000000000000LL, BUSY_QUEUEING_NS, BUSY_STALLS, std::mt19937_64(2)};
  // The primary's network clock jumps 50 ms (a PTP re-lock) at 30 s; 15 s
  // later the estimate is back on it.
  const ClockError error = run_clock(link, 45000000000LL, 60000000000LL, &clock, 30000000000LL, 50000000LL);
  std::printf("step: worst %.1f us after reseed, reseeds %u\n", error.worst_ns / 1e3, (unsigned) clock.reseeds());
  CHECK(clock.reseeds() >= 1);
  CHECK(error.worst_ns < 350000.0);
}

static void test_clock_needs_samples_to_lock() {
  PeerClock clock;
  int64_t offset = 0;
  CHECK(!clock.offset_at(0, &offset));
  for (int i = 0; i < (int) PeerClock::LOCK_SAMPLES - 1; i++) {
    const int64_t t = (int64_t) i * 125000000;
    clock.add_sample(t, t + 1000, t + 2000, t + 3000);
  }
  CHECK(!clock.locked());
  const int64_t t = 10 * 125000000LL;
  clock.add_sample(t, t + 1000, t + 2000, t + 3000);
  CHECK(clock.locked());
  CHECK(clock.offset_at(t, &offset));
  // Negative round trip (clock went backwards mid-exchange) is ignored.
  CHECK(!clock.add_sample(t + 10, t, t + 1000000, t + 20));
}

int main() {
  test_pcm_round_trip();
  test_pcm_rejects_corruption();
  test_state_round_trip();
  test_ping_pong_round_trip();
  test_serial_wrap();
  test_clock_needs_samples_to_lock();
  test_clock_tracks_drift();
  test_clock_calm_link();
  test_clock_reseeds_after_step();
  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
