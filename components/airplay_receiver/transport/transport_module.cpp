#include "transport_module.h"

namespace esphome {
namespace airplay_receiver {

TransportModule::TransportModule() = default;
TransportModule::~TransportModule() = default;

void TransportModule::setup() {
  // PENDING: real RTSP client/server setup.
}

void TransportModule::loop() {
  // PENDING: real RTSP connection handling.
}

int TransportModule::connection_count() const {
  return this->connection_count_;
}

}  // namespace airplay_receiver
}  // namespace esphome
