#pragma once

#include "esphome/core/component.h"

#include <string>

#include "audio/audio_output.h"
#include "audio/audio_receiver.h"
#include "crypto/crypto_module.h"
#include "transport/transport_module.h"

namespace esphome {
namespace airplay_receiver {

/**
 * AirPlay 2 (and AirPlay 1 / RAOP) receiver.
 *
 * Owns the CryptoModule (HomeKit pairing + ChaCha20-Poly1305 audio crypto),
 * the AirPlay2Transport (RTSP server on port 7000 + _airplay._tcp mDNS
 * advertisement), and the audio engine (audio_output I2S DAC + audio_receiver
 * pipeline). The transport drives the crypto module for PAIR-SETUP /
 * PAIR-VERIFY; the transport events then drive the audio receiver; the audio
 * output backend pulls decoded PCM and feeds the DAC + amp. Audio is exposed
 * to Home Assistant via media_player once the audio engine is running.
 */
class AirPlayReceiver : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_name(const std::string &name) { this->airplay_name_ = name; }
  void set_buffer_size(uint32_t buffer_size) { this->buffer_size_ = buffer_size; }

  /// Thread the I2S DAC + amp board wiring (from YAML) before the audio engine
  /// starts. bclk/lrclk/dout are PCM5100 I2S pins; amp is the amp-enable GPIO.
  void set_audio_config(int i2s_bclk_pin, int i2s_lrclk_pin, int i2s_dout_pin, int amp_enable_pin,
                        int sample_rate, bool amp_enable_inverted);

  /// Idle power-down for the amp-enable line (seconds, 0 disables). Default 60.
  void set_amp_idle_timeout(int seconds) { this->amp_idle_timeout_s_ = seconds; }

  /// Optional I2S MCLK/SCK pin (PCM5100 boards that don't self-strap). -1 = unused.
  void set_i2s_mclk(int pin) { this->i2s_mclk_pin_ = pin; }

  uint32_t get_buffer_size() const { return this->buffer_size_; }

 protected:
  /// Bridge a transport event callback (static C-style) to this instance.
  static void on_transport_event(TransportEvent event, const TransportEventData *data, void *user_data);
  /// React to a transport/control event (play/pause/volume/metadata/stream)
  /// and drive the audio receiver/output.
  void handle_transport_event(TransportEvent event, const TransportEventData *data);

  std::string airplay_name_{"AirPlay2"};
  uint32_t buffer_size_{1000000};

  // Audio board wiring (I2S PCM5100 + amp-enable).
  int i2s_bclk_pin_{-1};
  int i2s_lrclk_pin_{-1};
  int i2s_dout_pin_{-1};
  int i2s_mclk_pin_{-1};  // optional MCLK/SCK (-1 = unused)
  int amp_enable_pin_{-1};
  int sample_rate_{44100};
  bool amp_enable_inverted_{false};
  int amp_idle_timeout_s_{60};  // amp power-down after this many idle seconds (0 = off)

  // AirPlay 2 pairing + audio crypto.
  CryptoModule crypto_;
  // RTSP server + mDNS + control plane.
  AirPlay2Transport transport_;
};

}  // namespace airplay_receiver
}  // namespace esphome
