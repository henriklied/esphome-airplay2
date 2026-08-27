#include "airplay_receiver.h"

#include <algorithm>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_receiver";

void AirPlayReceiver::setup() {
  ESP_LOGCONFIG(TAG, "AirPlayReceiver '%s' buffer_size=%lu", this->airplay_name_.c_str(),
                (unsigned long) this->buffer_size_);

  // Load/generate the device Ed25519 identity (persisted via ESPHome prefs).
  this->crypto_.setup();
  ESP_LOGI(TAG, "Crypto ready (paired=%d)", this->crypto_.paired());

  // Audio output backend (I2S PCM5100 + amp enable) from YAML wiring.
  AudioOutputConfig oc{};
  oc.i2s_bclk_gpio = this->i2s_bclk_pin_;
  oc.i2s_lrclk_gpio = this->i2s_lrclk_pin_;
  oc.i2s_dout_gpio = this->i2s_dout_pin_;
  oc.amp_enable_gpio = this->amp_enable_pin_;
  oc.sample_rate = this->sample_rate_;
  oc.amp_enable_inverted = this->amp_enable_inverted_;
  oc.amp_idle_timeout_ms = (this->amp_idle_timeout_s_ > 0) ? (uint32_t) this->amp_idle_timeout_s_ * 1000U : 0;
  audio_output_set_config(oc);
  audio_output_init();
  ESP_LOGI(TAG, "Audio output init (bclk=%d lrclk=%d dout=%d amp=%d sr=%d idle_timeout=%us)", this->i2s_bclk_pin_,
           this->i2s_lrclk_pin_, this->i2s_dout_pin_, this->amp_enable_pin_, this->sample_rate_,
           this->amp_idle_timeout_s_);

  // Audio receiver + the CryptoModule inject the stream tasks decrypt through.
  audio_receiver_init();
  audio_receiver_set_crypto_module(&this->crypto_);

  // Start the _airplay._tcp advertisement + RTSP server and hook the audio
  // engine up to the transport events.
  this->transport_.setup(&this->crypto_, this->airplay_name_);
  this->transport_.register_event_callback(&AirPlayReceiver::on_transport_event, this);
}

void AirPlayReceiver::loop() {
  this->transport_.loop();
  // Audio is served by the receiver/output FreeRTOS tasks; nothing to do here.
}

void AirPlayReceiver::dump_config() {
  ESP_LOGCONFIG(TAG, "AirPlayReceiver name='%s'", this->airplay_name_.c_str());
  ESP_LOGCONFIG(TAG, "  buffer_size=%lu", (unsigned long) this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  transport started=%d stream_type=%lld", this->transport_.started(),
                (long long) this->transport_.current_stream_type());
  ESP_LOGCONFIG(TAG, "  audio i2s bclk=%d lrclk=%d dout=%d amp=%d sr=%d", this->i2s_bclk_pin_,
                this->i2s_lrclk_pin_, this->i2s_dout_pin_, this->amp_enable_pin_, this->sample_rate_);
}

void AirPlayReceiver::set_audio_config(int i2s_bclk_pin, int i2s_lrclk_pin, int i2s_dout_pin, int amp_enable_pin,
                                       int sample_rate, bool amp_enable_inverted) {
  this->i2s_bclk_pin_ = i2s_bclk_pin;
  this->i2s_lrclk_pin_ = i2s_lrclk_pin;
  this->i2s_dout_pin_ = i2s_dout_pin;
  this->amp_enable_pin_ = amp_enable_pin;
  if (sample_rate > 0) {
    this->sample_rate_ = sample_rate;
  }
  this->amp_enable_inverted_ = amp_enable_inverted;
}

void AirPlayReceiver::on_transport_event(TransportEvent event, const TransportEventData *data, void *user_data) {
  AirPlayReceiver *self = static_cast<AirPlayReceiver *>(user_data);
  if (self != nullptr) {
    self->handle_transport_event(event, data);
  }
}

void AirPlayReceiver::handle_transport_event(TransportEvent event, const TransportEventData *data) {
  switch (event) {
    case TRANSPORT_EVENT_AUDIO_CONFIGURED: {
      if (data == nullptr) {
        break;
      }
      // Configure the decoder format from the stream description.
      audio_format_t fmt = {};
      const char *codec = (data->audio.codec_type == 4 || data->audio.codec_type == 8) ? "AAC" : "AppleLossless";
      strncpy(fmt.codec, codec, sizeof(fmt.codec) - 1);
      fmt.sample_rate = data->audio.sample_rate > 0 ? data->audio.sample_rate : 44100;
      fmt.channels = data->audio.channels > 0 ? data->audio.channels : 2;
      fmt.bits_per_sample = data->audio.bits_per_sample > 0 ? data->audio.bits_per_sample : 16;
      fmt.frame_size = (data->audio.sample_rate > 0) ? fmt.channels * 2 : 352;
      audio_receiver_set_format(&fmt);
      ESP_LOGI(TAG, "audio: format codec=%s sr=%d ch=%d bps=%d", fmt.codec, fmt.sample_rate, fmt.channels,
               fmt.bits_per_sample);

      // ChaCha20-Poly1305 key material (shk preferred; ekey/session is derived
      // inside the receiver from the shared secret when shk is absent).
      if (data->audio.has_shk && data->audio.shk_len >= 16) {
        AudioEncrypt enc = {};
        enc.type = AudioEncryptType::CHACHA20_POLY1305;
        size_t n = std::min<size_t>(data->audio.shk_len, sizeof(enc.key));
        memcpy(enc.key, data->audio.shk, n);
        enc.key_len = n;
        audio_receiver_set_encryption(&enc);
      }

      audio_receiver_set_stream_type(static_cast<audio_stream_type_t>(data->audio.stream_type));
      // Realtime (type 96): data+control UDP ports. Buffered (type 103): the
      // TCP port handed by the transport.
      uint16_t tcp_port = (data->audio.stream_type == 103) ? data->audio.buffered_port : 0;
      esp_err_t err = audio_receiver_start_stream(data->audio.data_port, data->audio.control_port, tcp_port);
      ESP_LOGI(TAG, "audio: start_stream type=%lld data=%u ctrl=%u tcp=%u -> %s", (long long) data->audio.stream_type,
               data->audio.data_port, data->audio.control_port, tcp_port, esp_err_to_name(err));
      break;
    }
    case TRANSPORT_EVENT_ANCHOR:
      if (data != nullptr) {
        audio_receiver_set_anchor_time(data->anchor.clock_id, data->anchor.network_time_ns, data->anchor.rtp_time);
      }
      break;
    case TRANSPORT_EVENT_PLAYING:
      ESP_LOGI(TAG, "audio: PLAYING");
      audio_receiver_set_playing(true);
      audio_output_start();
      break;
    case TRANSPORT_EVENT_PAUSED:
      ESP_LOGI(TAG, "audio: PAUSED");
      audio_receiver_set_playing(false);
      audio_output_flush();
      break;
    case TRANSPORT_EVENT_VOLUME:
      audio_output_set_volume_q15(transport_volume_q15());
      break;
    case TRANSPORT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "audio: DISCONNECTED");
      audio_receiver_stop();
      audio_output_stop();
      break;
    case TRANSPORT_EVENT_METADATA:
    case TRANSPORT_EVENT_CLIENT_CONNECTED:
    default:
      break;
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
