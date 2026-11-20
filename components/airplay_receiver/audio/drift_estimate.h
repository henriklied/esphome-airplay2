#pragma once

// Drift estimate reported as playout: drift= (telemetry only; the servo in
// audio_scheduler.cpp does not read it).  One window per second of network
// time: the change in filtered playout error over the window, in ppm.
//
// Network time can step when PTP relocks onto a new timescale.  2026-11-20 a
// relock moved it back ~10 days; the old code waited for elapsed >= 1 s, which
// a backward step postpones until the clock catches up, so drift= froze at
// 20000 ppm for the rest of the session.  A window that runs backwards or far
// too long is now re-anchored instead of measured.
//
// Pure C++ with no platform dependencies so it builds on the host for tests.

#include <cstdint>

namespace esphome {
namespace airplay_receiver {

static constexpr int64_t DRIFT_WINDOW_NS = 1000000000LL;
static constexpr int64_t DRIFT_WINDOW_MAX_NS = 10 * DRIFT_WINDOW_NS;
/// Crystals sit within ~100 ppm; an estimate past this spans a timing step.
static constexpr int64_t DRIFT_ESTIMATE_MAX_PPM = 20000;
static constexpr int64_t Q16_ONE = 65536LL;
static constexpr int64_t PPM_PER_UNIT = 1000000LL;

struct DriftEstimate {
  int64_t reference_error_q16;
  /// 0 = no reference yet; the next update only anchors.
  int64_t reference_network_ns;
  int32_t ppm;
};

static inline void drift_estimate_anchor(DriftEstimate *estimate,
                                         int64_t error_q16,
                                         int64_t network_ns) {
  estimate->reference_error_q16 = error_q16;
  estimate->reference_network_ns = network_ns;
}

/// Feed one render's filtered playout error (q16 samples) at its midpoint
/// network time.  Updates estimate->ppm once per DRIFT_WINDOW_NS; a window
/// that runs backwards, exceeds DRIFT_WINDOW_MAX_NS or yields more than
/// DRIFT_ESTIMATE_MAX_PPM keeps the previous ppm and re-anchors.
static inline void drift_estimate_update(DriftEstimate *estimate,
                                         int64_t error_q16,
                                         int64_t network_ns,
                                         uint32_t sample_rate) {
  if (estimate->reference_network_ns == 0) {
    drift_estimate_anchor(estimate, error_q16, network_ns);
    return;
  }
  int64_t elapsed_ns = network_ns - estimate->reference_network_ns;
  if (elapsed_ns < 0 || elapsed_ns > DRIFT_WINDOW_MAX_NS) {
    drift_estimate_anchor(estimate, error_q16, network_ns);
    return;
  }
  if (elapsed_ns < DRIFT_WINDOW_NS) {
    return;
  }
  int64_t elapsed_samples = (elapsed_ns * (int64_t)sample_rate) / 1000000000LL;
  if (elapsed_samples > 0) {
    /* Divide in q16: truncating delta to whole samples first would quantise
     * the result to one sample per window, 22 ppm at 44.1 kHz. */
    int64_t delta_q16 = error_q16 - estimate->reference_error_q16;
    int64_t ppm = (delta_q16 * PPM_PER_UNIT) / (elapsed_samples * Q16_ONE);
    if (ppm <= DRIFT_ESTIMATE_MAX_PPM && ppm >= -DRIFT_ESTIMATE_MAX_PPM) {
      estimate->ppm = (int32_t)ppm;
    }
  }
  drift_estimate_anchor(estimate, error_q16, network_ns);
}

}  // namespace airplay_receiver
}  // namespace esphome
