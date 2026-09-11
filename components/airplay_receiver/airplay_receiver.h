#pragma once

#include "esphome/core/component.h"
#include "esphome/components/media_player/media_player.h"

#include <string>
#include <vector>

#include "audio/audio_control.h"
#include "audio/audio_dsp.h"
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
 * output backend pulls decoded PCM and feeds the DAC + amp.
 *
 * Audio and transport state is exposed to Home Assistant via the embedded
 * media_player (implied by inheriting media_player::MediaPlayer): volume,
 * play/pause/stop/transport state, and the reachable output channel modes
 * (STEREO/LEFT/RIGHT/MONO).
 */
class AirPlayReceiver : public Component, public media_player::MediaPlayer {
 public:
  /// Must run AFTER the network stack (AFTER_BLUETOOTH) AND after ESPHome's
  /// MDNSComponent (also AFTER_CONNECTION) so the RTSP socket + mDNS service
  /// adds happen with lwIP/mDNS up.
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_buffer_size(uint32_t buffer_size) { this->buffer_size_ = buffer_size; }

  /// Thread the I2S DAC + amp board wiring (from YAML) before the audio engine
  /// starts. bclk/lrclk/dout are PCM5100 I2S pins; amp is the amp-enable GPIO.
  void set_audio_config(int i2s_bclk_pin, int i2s_lrclk_pin, int i2s_dout_pin, int amp_enable_pin,
                        int sample_rate, bool amp_enable_inverted);

  /// Idle power-down for the amp-enable line (seconds, 0 disables). Default 60.
  void set_amp_idle_timeout(int seconds) { this->amp_idle_timeout_s_ = seconds; }

  /// Optional I2S MCLK/SCK pin (PCM5100 boards that don't self-strap). -1 = unused.
  void set_i2s_mclk(int pin) { this->i2s_mclk_pin_ = pin; }

  /// Output channel mode (0=STEREO 1=LEFT 2=RIGHT 3=MONO), from the YAML
  /// audio_channel_mode enum. Applied after audio_output_init().
  void set_audio_channel_mode(int mode) { this->audio_channel_mode_ = static_cast<audio_channel_mode_t>(mode); }

  // ---- output DSP (audio/audio_dsp.h) ----
  //
  // Direct AirPlay bypasses Music Assistant, so its per-player EQ never reaches
  // this board; the speaker correction has to run here instead. Filters are
  // collected from YAML in order and pushed to the DSP stage in setup(), once
  // the output sample rate is known.

  /// Append one biquad from the YAML `dsp.filters` list. `type` is the
  /// airplay_dsp_filter_type_t integer from the schema enum.
  void add_dsp_filter(int type, float frequency_hz, float q, float gain_db);

  /// Broadband gain ahead of the cascade, in dB. Negative values buy back the
  /// headroom a positive shelf spends.
  void set_dsp_preamp(float preamp_db);

  /// Bypass (false) or engage (true) the whole DSP stage. Bypass is bit-exact.
  void set_dsp_enabled(bool enabled);

  // Runtime tuning. These recompute coefficients, so they must run on the main
  // loop -- which is exactly where a YAML lambda runs, so a `number` entity can
  // drive them directly. Index is into the configured filter list.
  void set_dsp_filter_frequency(int index, float frequency_hz);
  void set_dsp_filter_q(int index, float q);
  void set_dsp_filter_gain(int index, float gain_db);

  float get_dsp_filter_frequency(int index) const;
  float get_dsp_filter_q(int index) const;
  float get_dsp_filter_gain(int index) const;
  float get_dsp_preamp() const { return this->dsp_preamp_db_; }
  bool get_dsp_enabled() const { return this->dsp_enabled_; }

  uint32_t get_buffer_size() const { return this->buffer_size_; }

  // ---- media_player::MediaPlayer implementations ----

  media_player::MediaPlayerTraits get_traits() override;
  bool is_muted() const override { return this->muted_; }

 protected:
  /// Bridge a transport event callback (static C-style) to this instance.
  static void on_transport_event(TransportEvent event, const TransportEventData *data, void *user_data);
  /// React to a transport/control event (play/pause/volume/metadata/stream)
  /// and drive the audio receiver/output + media_player state.
  void handle_transport_event(TransportEvent event, const TransportEventData *data);

  /// MediaPlayer command dispatch (HA service / automations).
  void control(const media_player::MediaPlayerCall &call) override;

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
  audio_channel_mode_t audio_channel_mode_{AUDIO_CHANNEL_STEREO};

  // Output DSP cascade, in YAML order. Mirrored here (rather than read back
  // from audio_dsp) so a runtime edit of one parameter keeps the rest.
  std::vector<AirPlayDspFilter> dsp_filters_;
  float dsp_preamp_db_{0.0f};
  bool dsp_enabled_{true};

  /// Push one mirrored filter into the DSP stage after a runtime edit.
  void publish_dsp_filter_(int index);

  /// Log a warning when the configured boost exceeds the preamp headroom.
  void warn_if_dsp_clips_();

  // Media-player mirror of the audio engine state.
  bool muted_{false};
  float cached_volume_{0.5f};
  // The transport event callback fires from the RTSP task; publish_state()
  // must run on the main loop. The RTSP task writes these and loop() publishes.
  bool audio_ready_{false};
  bool state_dirty_{false};
  media_player::MediaPlayerState desired_state_{media_player::MEDIA_PLAYER_STATE_NONE};

  // AirPlay 2 pairing + audio crypto.
  CryptoModule crypto_;
  // RTSP server + mDNS + control plane.
  AirPlay2Transport transport_;
};

}  // namespace airplay_receiver
}  // namespace esphome
