#pragma once

#include "esphome/core/component.h"

#include <string>

#include "crypto/crypto_module.h"
#include "transport/transport_module.h"

namespace esphome {
namespace airplay_receiver {

/**
 * AirPlay 2 (and AirPlay 1 / RAOP) receiver.
 *
 * Owns the CryptoModule (HomeKit pairing + ChaCha20-Poly1305 audio crypto) and
 * the AirPlay2Transport (RTSP server on port 7000 + _airplay._tcp mDNS
 * advertisement). The transport drives the crypto module for PAIR-SETUP /
 * PAIR-VERIFY and hands fully-configured streams to the audio engine through
 * transport events. Audio decoding/output is added by a later task; the
 * event/callback surface here is that wire-up point.
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
  /// Bridge a transport event callback (static C-style) to this instance.
  static void on_transport_event(TransportEvent event, const TransportEventData *data, void *user_data);
  /// React to a transport/control event (play/pause/volume/metadata/stream).
  void handle_transport_event(TransportEvent event, const TransportEventData *data);

  std::string airplay_name_{"AirPlay2"};
  uint32_t buffer_size_{1000000};

  // AirPlay 2 pairing + audio crypto.
  CryptoModule crypto_;
  // RTSP server + mDNS + control plane.
  AirPlay2Transport transport_;
};

}  // namespace airplay_receiver
}  // namespace esphome
