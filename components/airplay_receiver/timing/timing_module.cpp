#include "timing_module.h"

namespace esphome {
namespace airplay_receiver {

TimingModule::TimingModule() = default;
TimingModule::~TimingModule() = default;

void TimingModule::setup() {
  // PENDING: real clock/timing setup.
}

void TimingModule::loop() {
  // PENDING: real drift-servo / early-late gating.
}

bool TimingModule::synced() const {
  return this->synced_;
}

}  // namespace airplay_receiver
}  // namespace esphome
