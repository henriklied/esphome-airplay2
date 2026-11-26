#pragma once
// Stereo-pair relayed clock: the secondary's estimate of the primary's local
// clock, from PING/PONG exchanges over the peer link. The primary's network
// offset rides on each PONG separately (audio_peer.cpp).
//
// Header-only and free of ESP-IDF includes so tests/host can drive it with a
// simulated link.
//
// Each exchange gives the four NTP timestamps: t1/t4 on the secondary's local
// clock, t2/t3 on the primary's. offset = ((t2 - t1) + (t3 - t4)) / 2 is
// exact for a symmetric path; asymmetry shows as error of half the
// difference, and Wi-Fi queueing makes that difference large on a slow
// exchange. So only exchanges close to the recent minimum round trip are
// kept, and the estimate is a least-squares line through the kept ones:
// offset and rate together, so it follows the two crystals' drift instead of
// lagging behind it between samples.
//
// Not thread-safe. The owner serialises add_sample() against offset_at().

#include <cmath>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {
namespace peer {

class PeerClock {
 public:
  // Round trips remembered for the minimum: 3.2 s at 10 exchanges/s. Long
  // enough to see past a burst of queueing, short enough to follow a roam.
  static constexpr int RTT_WINDOW = 32;
  // An exchange is kept if its round trip is within this of the minimum, so
  // its path asymmetry, and the error it carries, is at most half of it.
  static constexpr int64_t RTT_SLACK_NS = 400000;
  // Kept exchanges in the fit, and the oldest one allowed. A longer line
  // averages more exchanges; crystals drift with temperature over minutes,
  // so a minute is still straight.
  static constexpr int FIT_CAPACITY = 128;
  static constexpr int64_t FIT_MAX_AGE_NS = 60000000000LL;
  // Kept exchanges before the estimate is trusted.
  static constexpr int LOCK_SAMPLES = 4;
  // Rate prior: a fit spanning less than this leans towards rate 0 instead
  // of trusting a slope drawn through a few noisy points.
  static constexpr double RATE_PRIOR_SPAN_NS = 5e9;
  // A kept exchange this far from the line is an outlier...
  static constexpr int64_t OUTLIER_NS = 5000000;
  // ...and this many in a row means the primary's clock stepped: start over.
  static constexpr int OUTLIER_RUN_RESEED = 4;
  // Crystal tolerance is +-20 ppm a side; a short noisy fit can claim far
  // more, so clamp it.
  static constexpr double MAX_RATE = 200e-6;

  void reset() { *this = PeerClock(); }

  /**
   * Feed one exchange.
   *
   * @param t1_local_ns   secondary local time at PING send
   * @param t2_network_ns primary time at PING receipt
   * @param t3_network_ns primary time at PONG send
   * @param t4_local_ns   secondary local time at PONG receipt
   * @return true if the exchange was kept
   */
  bool add_sample(int64_t t1_local_ns, int64_t t2_network_ns, int64_t t3_network_ns, int64_t t4_local_ns) {
    const int64_t rtt = (t4_local_ns - t1_local_ns) - (t3_network_ns - t2_network_ns);
    if (rtt < 0) {
      return false;
    }
    this->rtts_[this->rtt_next_] = rtt;
    this->rtt_next_ = (this->rtt_next_ + 1) % RTT_WINDOW;
    if (this->rtt_count_ < RTT_WINDOW) {
      this->rtt_count_++;
    }
    this->last_rtt_ns_ = rtt;
    int64_t min_rtt = rtt;
    for (int i = 0; i < this->rtt_count_; i++) {
      if (this->rtts_[i] < min_rtt) {
        min_rtt = this->rtts_[i];
      }
    }
    this->min_rtt_ns_ = min_rtt;
    if (rtt > min_rtt + RTT_SLACK_NS) {
      return false;
    }

    const int64_t at_ns = t1_local_ns + (t4_local_ns - t1_local_ns) / 2;
    const int64_t measured = ((t2_network_ns - t1_local_ns) + (t3_network_ns - t4_local_ns)) / 2;

    if (this->fit_count_ > 0 && at_ns <= this->kept_[this->fit_count_ - 1].at_ns) {
      return false;
    }
    if (this->locked()) {
      int64_t predicted = 0;
      (void) this->offset_at(at_ns, &predicted);
      const int64_t error = measured - predicted;
      if (error > OUTLIER_NS || error < -OUTLIER_NS) {
        if (++this->outlier_run_ < OUTLIER_RUN_RESEED) {
          return false;
        }
        this->reseeds_++;
        this->fit_count_ = 0;
      }
    }
    this->outlier_run_ = 0;

    this->keep_(at_ns, measured, rtt);
    this->refit_();
    return true;
  }

  bool locked() const { return this->fit_count_ >= LOCK_SAMPLES; }

  /// remote = local + offset, at local time `local_ns`. False until locked.
  bool offset_at(int64_t local_ns, int64_t *offset_ns) const {
    if (!this->locked()) {
      return false;
    }
    *offset_ns = offset_on(this->line(), local_ns);
    return true;
  }

  /// The fitted line, for a reader that must not pay for a refit: network =
  /// local + offset_ns + rate * (local - at_ns).
  struct Line {
    bool valid;
    int64_t at_ns;
    int64_t offset_ns;
    double rate;
  };
  Line line() const { return Line{this->locked(), this->at_ns_, this->offset_ns_, this->rate_}; }

  static int64_t offset_on(const Line &line, int64_t local_ns) {
    return line.offset_ns + (int64_t) (line.rate * (double) (local_ns - line.at_ns));
  }

  /// RMS distance of the kept exchanges from the line: the measurement noise
  /// the fit averages down. Its error at the newest point is roughly this
  /// over sqrt(samples), times ~2 for extrapolating past the centre.
  int64_t residual_rms_ns() const { return this->residual_rms_ns_; }

  int64_t min_rtt_ns() const { return this->min_rtt_ns_; }
  int64_t last_rtt_ns() const { return this->last_rtt_ns_; }
  double rate() const { return this->rate_; }
  int samples() const { return this->fit_count_; }
  uint32_t reseeds() const { return this->reseeds_; }

 private:
  struct Kept {
    int64_t at_ns;
    int64_t offset_ns;
    int64_t rtt_ns;
  };

  // Append, oldest first. Drops what has aged out, and what a lower minimum
  // since shows to have been a slow exchange: the first few exchanges are
  // kept before the minimum means anything, and would otherwise bias the
  // line for its whole span.
  void keep_(int64_t at_ns, int64_t offset_ns, int64_t rtt_ns) {
    int kept = 0;
    for (int i = 0; i < this->fit_count_; i++) {
      const Kept &k = this->kept_[i];
      if (at_ns - k.at_ns > FIT_MAX_AGE_NS || k.rtt_ns > this->min_rtt_ns_ + RTT_SLACK_NS) {
        continue;
      }
      this->kept_[kept++] = k;
    }
    if (kept == FIT_CAPACITY) {
      for (int i = 1; i < kept; i++) {
        this->kept_[i - 1] = this->kept_[i];
      }
      kept--;
    }
    this->kept_[kept++] = Kept{at_ns, offset_ns, rtt_ns};
    this->fit_count_ = kept;
  }

  // Least squares through the kept exchanges, centred on their means so the
  // sums stay small enough for doubles to hold to the nanosecond.
  void refit_() {
    const int n = this->fit_count_;
    const int64_t at_ref = this->kept_[0].at_ns;
    const int64_t offset_ref = this->kept_[0].offset_ns;
    double mean_x = 0.0;
    double mean_y = 0.0;
    for (int i = 0; i < n; i++) {
      mean_x += (double) (this->kept_[i].at_ns - at_ref);
      mean_y += (double) (this->kept_[i].offset_ns - offset_ref);
    }
    mean_x /= n;
    mean_y /= n;
    double sxx = 0.0;
    double sxy = 0.0;
    for (int i = 0; i < n; i++) {
      const double dx = (double) (this->kept_[i].at_ns - at_ref) - mean_x;
      const double dy = (double) (this->kept_[i].offset_ns - offset_ref) - mean_y;
      sxx += dx * dx;
      sxy += dx * dy;
    }
    // Ridge term: the sxx a fit of n points evenly spread over the prior span
    // would have. Negligible once the real span is several times longer.
    const double prior = (double) n * RATE_PRIOR_SPAN_NS * RATE_PRIOR_SPAN_NS / 12.0;
    double rate = sxy / (sxx + prior);
    if (rate > MAX_RATE) {
      rate = MAX_RATE;
    } else if (rate < -MAX_RATE) {
      rate = -MAX_RATE;
    }
    this->rate_ = rate;
    this->at_ns_ = at_ref + (int64_t) mean_x;
    this->offset_ns_ = offset_ref + (int64_t) mean_y;
    double sum_squares = 0.0;
    for (int i = 0; i < n; i++) {
      const double residual = (double) (this->kept_[i].offset_ns - this->offset_ns_) -
                              rate * (double) (this->kept_[i].at_ns - this->at_ns_);
      sum_squares += residual * residual;
    }
    this->residual_rms_ns_ = (int64_t) std::sqrt(sum_squares / n);
  }

  int64_t rtts_[RTT_WINDOW] = {};
  int rtt_next_{0};
  int rtt_count_{0};
  int64_t min_rtt_ns_{0};
  int64_t last_rtt_ns_{0};

  Kept kept_[FIT_CAPACITY] = {};
  int fit_count_{0};

  // The fitted line: offset_ns_ at local at_ns_, changing at rate_.
  int64_t at_ns_{0};
  int64_t offset_ns_{0};
  double rate_{0.0};
  int64_t residual_rms_ns_{0};
  int outlier_run_{0};
  uint32_t reseeds_{0};
};

}  // namespace peer
}  // namespace airplay_receiver
}  // namespace esphome
