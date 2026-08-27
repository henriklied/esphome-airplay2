#pragma once
// airplay_receiver timing module skeleton (place-holder).
//
// Holds the playout clock / PTP / drift-servo / early-late timing engine that
// keeps audio synchronized. Implemented by a later task. Self-contained.

#include <cstddef>

namespace esphome {
namespace airplay_receiver {

/**
 * Timing shell: playout clock + jitter-buffer timing.
 * (PENDING: NTP/PTP clock, one-sample drift servo, early/late gating, see
 * UPSTREAMING.md.)
 */
class TimingModule {
 public:
  TimingModule();
  ~TimingModule();

  void setup();
  void loop();

  bool synced() const;

 private:
  bool synced_{false};
};

}  // namespace airplay_receiver
}  // namespace esphome
