#include "airplay_receiver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_receiver";

void AirPlayReceiver::setup() {
  // The entity name (config `name:`, from the media_player schema) is the user
  // visible AirPlay device name. Fall back if it is not populated yet.
  std::string device_name = this->get_name().c_str();
  if (device_name.empty()) {
    device_name = "AirPlay2";
  }

  ESP_LOGCONFIG(TAG, "AirPlayReceiver '%s' buffer_size=%lu", device_name.c_str(), (unsigned long) this->buffer_size_);

  // Load/generate the device Ed25519 identity (persisted via ESPHome prefs).
  this->crypto_.setup();
  ESP_LOGI(TAG, "Crypto ready (paired=%d)", this->crypto_.paired());

  // Audio output backend (I2S PCM5100 + amp enable) from YAML wiring.
  AudioOutputConfig oc{};
  oc.i2s_bclk_gpio = this->i2s_bclk_pin_;
  oc.i2s_mclk_gpio = this->i2s_mclk_pin_;
  oc.i2s_lrclk_gpio = this->i2s_lrclk_pin_;
  oc.i2s_dout_gpio = this->i2s_dout_pin_;
  oc.amp_enable_gpio = this->amp_enable_pin_;
  oc.sample_rate = this->sample_rate_;
  oc.amp_enable_inverted = this->amp_enable_inverted_;
  oc.amp_idle_timeout_ms = (this->amp_idle_timeout_s_ > 0) ? (uint32_t) this->amp_idle_timeout_s_ * 1000U : 0;
  audio_output_set_config(oc);
  audio_output_init();
  // Apply the configured output channel mode (STEREO/LEFT/RIGHT/MONO).
  audio_output_set_channel_mode(this->audio_channel_mode_);
  // DSP last: audio_output_init() has just told the stage the output rate, so
  // the coefficients are designed against the rate actually being clocked.
  audio_dsp_set_preamp_db(this->dsp_preamp_db_);
  audio_dsp_set_enabled(this->dsp_enabled_);
  if (!this->dsp_filters_.empty()) {
    audio_dsp_set_filters(this->dsp_filters_.data(), this->dsp_filters_.size());
    this->warn_if_dsp_clips_();
  }
  // At INFO, not CONFIG: these boards run `logger: level: INFO` to keep log
  // formatting off the core that feeds I2S, and INFO is below CONFIG -- logging
  // the cascade only from dump_config would hide it on every board that matters.
  audio_dsp_log_cascade(TAG, ESPHOME_LOG_LEVEL_INFO);
  ESP_LOGI(TAG, "Audio output init (bclk=%d lrclk=%d dout=%d amp=%d sr=%d idle_timeout=%us)",
           this->i2s_bclk_pin_, this->i2s_lrclk_pin_, this->i2s_dout_pin_, this->amp_enable_pin_, this->sample_rate_,
           this->amp_idle_timeout_s_);

  // Audio receiver + the CryptoModule inject the stream tasks decrypt through.
  audio_receiver_init();
  audio_receiver_set_crypto_module(&this->crypto_);

  // Start the _airplay._tcp advertisement + RTSP server and hook the audio
  // engine up to the transport events. At AFTER_CONNECTION the network stack
  // and ESPHome's mDNS are already initialized.
  this->transport_.setup(&this->crypto_, device_name);
  this->transport_.register_event_callback(&AirPlayReceiver::on_transport_event, this);

  // Reflect the initial transport state to Home Assistant.
  this->audio_ready_ = true;
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
  this->volume = 0.5f;
  this->publish_state();
}

void AirPlayReceiver::loop() {
  this->transport_.loop();

  // Media-player state/volume are produced from the RTSP task (via the
  // transport event callback), so publish them from the main loop here to stay
  // thread-safe. publish_state() must only run on the loop thread, and the
  // RTSP task only sets desired_state_/state_dirty_ (see airplay_receiver.h).
  if (!this->audio_ready_) {
    return;
  }
  if (this->state_dirty_) {
    this->state = this->desired_state_;
    this->state_dirty_ = false;
    this->publish_state();
  }
  int volume_pct = airplay_audio_get_volume();
  if (volume_pct < 0) {
    volume_pct = 0;
  }
  float volume = static_cast<float>(volume_pct) / 100.0f;
  if (volume != this->volume) {
    this->volume = volume;
    this->publish_state();
  }
}

void AirPlayReceiver::dump_config() {
  const char *name = this->get_name().c_str();
  if (name == nullptr || *name == '\0') {
    name = "AirPlay2";
  }
  ESP_LOGCONFIG(TAG, "AirPlayReceiver name='%s'", name);
  ESP_LOGCONFIG(TAG, "  buffer_size=%lu", (unsigned long) this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  transport started=%d stream_type=%lld", this->transport_.started(),
                (long long) this->transport_.current_stream_type());
  ESP_LOGCONFIG(TAG, "  audio i2s bclk=%d lrclk=%d dout=%d amp=%d sr=%d", this->i2s_bclk_pin_, this->i2s_lrclk_pin_,
                this->i2s_dout_pin_, this->amp_enable_pin_, this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  audio channel mode=%d", (int) this->audio_channel_mode_);
  audio_dsp_log_cascade(TAG, ESPHOME_LOG_LEVEL_CONFIG);
  const char *track = airplay_audio_get_track_title();
  if (track != nullptr && *track != '\0') {
    ESP_LOGCONFIG(TAG, "  track='%s'", track);
  }
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

void AirPlayReceiver::add_dsp_filter(int type, float frequency_hz, float q, float gain_db) {
  AirPlayDspFilter filter;
  filter.type = static_cast<airplay_dsp_filter_type_t>(type);
  filter.frequency_hz = frequency_hz;
  filter.q = q;
  filter.gain_db = gain_db;
  // Collected only; setup() publishes the whole cascade once the output rate is
  // known, so the coefficients are never designed against a guessed rate.
  this->dsp_filters_.push_back(filter);
}

void AirPlayReceiver::set_dsp_preamp(float preamp_db) {
  this->dsp_preamp_db_ = preamp_db;
  audio_dsp_set_preamp_db(preamp_db);
}

void AirPlayReceiver::set_dsp_enabled(bool enabled) {
  this->dsp_enabled_ = enabled;
  audio_dsp_set_enabled(enabled);
}

void AirPlayReceiver::publish_dsp_filter_(int index) {
  audio_dsp_set_filter((size_t) index, this->dsp_filters_[(size_t) index]);
}

/// True when `index` addresses a configured filter. Runtime setters are driven
/// from YAML lambdas, where an index typo is a silent no-op without this.
static bool dsp_index_valid(const char *what, int index, size_t count) {
  if (index >= 0 && (size_t) index < count) {
    return true;
  }
  ESP_LOGW(TAG, "DSP %s: filter index %d out of range (%u configured)", what, index, (unsigned) count);
  return false;
}

void AirPlayReceiver::set_dsp_filter_frequency(int index, float frequency_hz) {
  if (!dsp_index_valid("frequency", index, this->dsp_filters_.size())) {
    return;
  }
  this->dsp_filters_[(size_t) index].frequency_hz = frequency_hz;
  this->publish_dsp_filter_(index);
}

void AirPlayReceiver::set_dsp_filter_q(int index, float q) {
  if (!dsp_index_valid("q", index, this->dsp_filters_.size())) {
    return;
  }
  this->dsp_filters_[(size_t) index].q = q;
  this->publish_dsp_filter_(index);
}

void AirPlayReceiver::set_dsp_filter_gain(int index, float gain_db) {
  if (!dsp_index_valid("gain", index, this->dsp_filters_.size())) {
    return;
  }
  this->dsp_filters_[(size_t) index].gain_db = gain_db;
  this->publish_dsp_filter_(index);
}

float AirPlayReceiver::get_dsp_filter_frequency(int index) const {
  return (index >= 0 && (size_t) index < this->dsp_filters_.size()) ? this->dsp_filters_[(size_t) index].frequency_hz
                                                                    : 0.0f;
}

float AirPlayReceiver::get_dsp_filter_q(int index) const {
  return (index >= 0 && (size_t) index < this->dsp_filters_.size()) ? this->dsp_filters_[(size_t) index].q : 0.0f;
}

float AirPlayReceiver::get_dsp_filter_gain(int index) const {
  return (index >= 0 && (size_t) index < this->dsp_filters_.size()) ? this->dsp_filters_[(size_t) index].gain_db : 0.0f;
}

void AirPlayReceiver::warn_if_dsp_clips_() {
  // Conservative bound: a boosting section can contribute its full gain, so the
  // sum of the positive gains is the worst case the preamp has to absorb. Only
  // a warning -- the DSP stage clamps rather than wrapping, so the failure mode
  // is distortion at high volume, not noise.
  float boost_db = 0.0f;
  for (const auto &filter : this->dsp_filters_) {
    if (filter.gain_db > 0.0f) {
      boost_db += filter.gain_db;
    }
  }
  const float headroom_db = boost_db + this->dsp_preamp_db_;
  if (headroom_db > 0.0f) {
    ESP_LOGW(TAG, "DSP boosts up to %+.1f dB with preamp %+.1f dB: peaks above -%.1f dBFS will clip. "
                  "Set dsp.preamp to %+.1f dB to stay clear.",
             boost_db, this->dsp_preamp_db_, headroom_db, -boost_db);
  }
}

media_player::MediaPlayerTraits AirPlayReceiver::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  // This is an AirPlay receiver: it does not play a local URL or browse media,
  // and it cannot announce (mixing). Drop the base defaults we do not support.
  traits.clear_feature_flags(media_player::MediaPlayerEntityFeature::PLAY_MEDIA |
                             media_player::MediaPlayerEntityFeature::BROWSE_MEDIA |
                             media_player::MediaPlayerEntityFeature::MEDIA_ANNOUNCE);
  traits.add_feature_flags(media_player::MediaPlayerEntityFeature::PLAY |
                           media_player::MediaPlayerEntityFeature::PAUSE |
                           media_player::MediaPlayerEntityFeature::STOP |
                           media_player::MediaPlayerEntityFeature::VOLUME_SET |
                           media_player::MediaPlayerEntityFeature::VOLUME_STEP |
                           media_player::MediaPlayerEntityFeature::VOLUME_MUTE);
  return traits;
}

void AirPlayReceiver::control(const media_player::MediaPlayerCall &call) {
  // Ha volume (0.0-1.0) -> audio engine percent (0-100).
  if (auto volume = call.get_volume(); volume.has_value()) {
    uint8_t vol_pct = static_cast<uint8_t>(std::round(volume.value() * 100.0f));
    airplay_audio_set_volume(vol_pct);
    this->volume = volume.value();
    this->cached_volume_ = this->volume;
    this->publish_state();
  }

  auto command = call.get_command();
  if (!command.has_value()) {
    return;
  }
  bool playing;
  switch (command.value()) {
    case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
      playing = airplay_audio_is_playing();
      if (playing) {
        airplay_audio_pause();
        this->desired_state_ = media_player::MEDIA_PLAYER_STATE_PAUSED;
      } else {
        airplay_audio_play();
        this->desired_state_ = media_player::MEDIA_PLAYER_STATE_PLAYING;
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PLAY:
      airplay_audio_play();
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_PLAYING;
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
      airplay_audio_pause();
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_PAUSED;
      break;
    case media_player::MEDIA_PLAYER_COMMAND_STOP:
      airplay_audio_stop();
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_IDLE;
      break;
    case media_player::MEDIA_PLAYER_COMMAND_MUTE:
      this->muted_ = true;
      airplay_audio_set_volume(0);
      break;
    case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
      this->muted_ = false;
      airplay_audio_set_volume(static_cast<uint8_t>(std::round(this->cached_volume_ * 100.0f)));
      break;
    case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
      float v = std::min(1.0f, this->volume + 0.05f);
      airplay_audio_set_volume(static_cast<uint8_t>(std::round(v * 100.0f)));
      this->volume = v;
      this->cached_volume_ = v;
      break;
    }
    case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
      float v = std::max(0.0f, this->volume - 0.05f);
      airplay_audio_set_volume(static_cast<uint8_t>(std::round(v * 100.0f)));
      this->volume = v;
      this->cached_volume_ = v;
      break;
    }
    case media_player::MEDIA_PLAYER_COMMAND_TURN_ON:
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_ON;
      break;
    case media_player::MEDIA_PLAYER_COMMAND_TURN_OFF:
      airplay_audio_stop();
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_OFF;
      break;
    default:
      break;
  }
  this->publish_state();
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
      bool is_aac = (data->audio.codec_type == 4 || data->audio.codec_type == 8);
      const char *codec = is_aac ? "AAC" : "AppleLossless";
      strncpy(fmt.codec, codec, sizeof(fmt.codec) - 1);
      fmt.sample_rate = data->audio.sample_rate > 0 ? data->audio.sample_rate : 44100;
      fmt.channels = data->audio.channels > 0 ? data->audio.channels : 2;
      // DEFENSE: the engine, decoder and resampler are all stereo
      // (AUDIO_MAX_CHANNELS == 2). Clamp an over-large SDP channel count so a
      // malformed ANNOUNCE cannot feed a >2ch format downstream.
      if (fmt.channels > 2) {
        fmt.channels = 2;
      }
      fmt.bits_per_sample = data->audio.bits_per_sample > 0 ? data->audio.bits_per_sample : 16;
      // frame_size is samples-per-frame (ALAC 352, AAC 1024), not a byte count.
      fmt.frame_size = data->audio.frame_size > 0 ? data->audio.frame_size : (is_aac ? 1024 : 352);
      audio_receiver_set_format(&fmt);
      // Realtime streams play out latencyMin after the anchor (11025 = 250 ms);
      // buffered streams play at the anchor (0). Mirrors upstream SETUP.
      audio_receiver_set_playout_latency_samples(data->audio.playout_latency_samples);
      ESP_LOGI(TAG, "audio: format codec=%s sr=%d ch=%d bps=%d frame_size=%d latency=%lu", fmt.codec, fmt.sample_rate,
               fmt.channels, fmt.bits_per_sample, fmt.frame_size, (unsigned long) data->audio.playout_latency_samples);

      // ChaCha20-Poly1305 stream key, resolved by the transport via the
      // shk/ekey/derive chain (CryptoModule::configure_audio_encryption).
      if (data->audio.has_encrypt) {
        AudioEncrypt enc = {};
        enc.type = AudioEncryptType::CHACHA20_POLY1305;
        size_t n = std::min<size_t>(data->audio.encrypt_key_len, sizeof(enc.key));
        memcpy(enc.key, data->audio.encrypt_key, n);
        enc.key_len = n;
        audio_receiver_set_encryption(&enc);
      }

      audio_receiver_set_stream_type(static_cast<audio_stream_type_t>(data->audio.stream_type));
      // Realtime (type 96): data+control UDP ports. Buffered (type 103): the
      // TCP port handed by the transport.
      uint16_t tcp_port = (data->audio.stream_type == 103) ? data->audio.buffered_port : 0;
      // Arm NACK retransmission before the stream starts. Without this the
      // engine keeps retransmit_enabled false and send_resend_request() returns
      // early, so every lost RTP packet is concealed instead of re-requested.
      audio_receiver_set_client_control(data->audio.client_ip, data->audio.client_control_port);
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
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_PLAYING;
      this->state_dirty_ = true;
      break;
    case TRANSPORT_EVENT_PAUSED:
      ESP_LOGI(TAG, "audio: PAUSED");
      audio_receiver_set_playing(false);
      audio_output_flush();
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_PAUSED;
      this->state_dirty_ = true;
      break;
    case TRANSPORT_EVENT_VOLUME:
      audio_output_set_volume_q15(transport_volume_q15());
      break;
    case TRANSPORT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "audio: DISCONNECTED");
      audio_receiver_stop();
      audio_output_stop();
      this->desired_state_ = media_player::MEDIA_PLAYER_STATE_IDLE;
      this->state_dirty_ = true;
      break;
    case TRANSPORT_EVENT_FLUSH:
      if (data != nullptr && data->flush.flush_until_ts > 0) {
        ESP_LOGI(TAG, "audio: deferred flush until ts=%lu", (unsigned long) data->flush.flush_until_ts);
        audio_receiver_set_deferred_flush(data->flush.flush_until_ts);
      } else {
        // Immediate seek-flush (upstream handle_flush): re-preroll the engine
        // AND flush the I2S output so the previous track's DMA tail stops and
        // the cursor resets before the new stream's frames arrive.
        ESP_LOGI(TAG, "audio: seek flush");
        audio_receiver_seek_flush();
        audio_output_flush();
      }
      break;
    case TRANSPORT_EVENT_METADATA: {
      // The RTSP layer parsed the DAAP tags and emitted this event. Consume it
      // and publish the track title into the audio control surface so it is
      // actually retained (dump_config) instead of being dead plumbing. (This
      // ESPHome media_player has no title/artist fields, so we hold the title
      // in the audio layer and log the rest.)
      if (data != nullptr) {
        if (data->metadata.title[0] != '\0') {
          airplay_audio_set_track_title(data->metadata.title);
        }
        ESP_LOGI(TAG, "audio: track title='%s' artist='%s' album='%s'", data->metadata.title,
                 data->metadata.artist, data->metadata.album);
      }
      break;
    }
    case TRANSPORT_EVENT_CLIENT_CONNECTED:
    default:
      break;
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
