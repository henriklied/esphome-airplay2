#pragma once
// airplay_receiver transport module skeleton (place-holder).
//
// Holds the RTSP client/server transport that will carry the AirPlay 2
// stream. Implemented by a later task. Self-contained so esphome.h can
// auto-include it; no cross-module or IDF includes yet.

#include <cstddef>

namespace esphome {
namespace airplay_receiver {

/**
 * Transport shell: owns RTSP connection/server state.
 * (PENDING: real RTSP client/server logic, see UPSTREAMING.md.)
 */
class TransportModule {
 public:
  TransportModule();
  ~TransportModule();

  void setup();
  void loop();

  int connection_count() const;

 private:
  int connection_count_{0};
};

}  // namespace airplay_receiver
}  // namespace esphome
