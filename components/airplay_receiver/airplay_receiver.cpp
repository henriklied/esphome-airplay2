#include "airplay_receiver.h"
#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_receiver";

void AirPlayReceiver::setup() {
  ESP_LOGCONFIG(TAG, "AirPlayReceiver '%s' buffer_size=%lu", this->airplay_name_.c_str(),
                (unsigned long) this->buffer_size_);
}

void AirPlayReceiver::loop() {
  // Task 1 stub: no audio pipeline yet.
}

void AirPlayReceiver::dump_config() {
  ESP_LOGCONFIG(TAG, "AirPlayReceiver name='%s'", this->airplay_name_.c_str());
  ESP_LOGCONFIG(TAG, "  buffer_size=%lu", (unsigned long) this->buffer_size_);
}

}  // namespace airplay_receiver
}  // namespace esphome
