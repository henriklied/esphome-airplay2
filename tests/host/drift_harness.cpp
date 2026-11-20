// Host checks for audio/drift_estimate.h. Exits non-zero with a message on the
// first failure. Driven by tests/test_drift_estimate.py.

#include "audio/drift_estimate.h"

#include <cstdio>
#include <cstdlib>

using esphome::airplay_receiver::DRIFT_WINDOW_MAX_NS;
using esphome::airplay_receiver::DRIFT_WINDOW_NS;
using esphome::airplay_receiver::DriftEstimate;
using esphome::airplay_receiver::drift_estimate_update;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                   __LINE__, #cond);                                   \
      std::exit(1);                                                    \
    }                                                                  \
  } while (0)

static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr int64_t RENDER_NS = 352LL * 1000000000LL / SAMPLE_RATE;
static constexpr int64_t START_NS = 1700000000LL * 1000000000LL;
static constexpr double Q16 = 65536.0;
/* 2026-11-20 relock: network time moved back ~10 days. */
static constexpr int64_t RELOCK_STEP_NS = -863948610318621LL;

/* Renders for `seconds` with the playout error walking at `ppm`, starting
 * from `error_samples`; returns the error reached. */
static double play(DriftEstimate *estimate, int64_t *network_ns, double ppm,
                   double error_samples, double seconds) {
  for (int64_t t = 0; t < (int64_t)(seconds * 1e9); t += RENDER_NS) {
    drift_estimate_update(estimate, (int64_t)(error_samples * Q16), *network_ns,
                          SAMPLE_RATE);
    *network_ns += RENDER_NS;
    error_samples += ppm * 1e-6 * SAMPLE_RATE * RENDER_NS / 1e9;
  }
  return error_samples;
}

static bool near(int32_t got, int32_t want) { return got >= want - 2 && got <= want + 2; }

static void steady_drift_is_measured() {
  DriftEstimate estimate = {};
  int64_t now = START_NS;
  play(&estimate, &now, 21.0, 0.0, 3.0);
  CHECK(near(estimate.ppm, 21));
}

static void backward_step_does_not_freeze() {
  DriftEstimate estimate = {};
  int64_t now = START_NS;
  double error = play(&estimate, &now, 21.0, 0.0, 3.0);
  now += RELOCK_STEP_NS;
  error = play(&estimate, &now, -35.0, error, 3.0);
  CHECK(near(estimate.ppm, -35));
}

static void forward_step_is_not_measured() {
  DriftEstimate estimate = {};
  int64_t now = START_NS;
  double error = play(&estimate, &now, 21.0, 0.0, 3.0);
  now += DRIFT_WINDOW_MAX_NS + DRIFT_WINDOW_NS;
  /* An error jump across the gap would read as a wild ppm if measured. */
  play(&estimate, &now, 21.0, error + 5000.0, 0.5);
  CHECK(near(estimate.ppm, 21));
  play(&estimate, &now, 21.0, error + 5000.0, 2.0);
  CHECK(near(estimate.ppm, 21));
}

static void error_step_keeps_previous_estimate() {
  DriftEstimate estimate = {};
  int64_t now = START_NS;
  double error = play(&estimate, &now, 21.0, 0.0, 3.0);
  /* The 16:48:50 render: filtered error jumped by -119951 samples. */
  error = play(&estimate, &now, 21.0, error - 119951.0, 1.2);
  CHECK(near(estimate.ppm, 21));
  play(&estimate, &now, 21.0, error, 2.0);
  CHECK(near(estimate.ppm, 21));
}

int main() {
  steady_drift_is_measured();
  backward_step_does_not_freeze();
  forward_step_is_not_measured();
  error_step_keeps_previous_estimate();
  std::puts("ok");
  return 0;
}
