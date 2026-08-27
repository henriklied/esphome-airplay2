#pragma once

#include "esphome/core/component.h"
#include <string>

namespace esphome {
namespace airplay_receiver {

/**
 * AirPlay 2 (and AirPlay 1 / RAOP) receiver.
 *
 * Task 1 scaffold: registers as an ESPHome Component so the integration
 * pipeline (config validation + codegen) is green. Audio handling, the
 * centralized allocator, and the media_player surface are added by later
 * tasks.
 */
class AirPlayReceiver : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_name(const std::string &name) { this->airplay_name_ = name; }
  void set_buffer_size(uint32_t buffer_size) { this->buffer_size_ = buffer_size; }

  uint32_t get_buffer_size() const { return this->buffer_size_; }

 protected:
  std::string airplay_name_{"AirPlay2"};
  uint32_t buffer_size_{1000000};
};

}  // namespace airplay_receiver
}  // namespace esphome
