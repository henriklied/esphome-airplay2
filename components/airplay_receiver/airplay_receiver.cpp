#include "airplay_receiver.h"

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

  // Start the _airplay._tcp advertisement + RTSP server on port 7000 and hook
  // the audio-engine event callback.
  this->transport_.setup(&this->crypto_, this->airplay_name_);
  this->transport_.register_event_callback(&AirPlayReceiver::on_transport_event, this);
}

void AirPlayReceiver::loop() { this->transport_.loop(); }

void AirPlayReceiver::dump_config() {
  ESP_LOGCONFIG(TAG, "AirPlayReceiver name='%s'", this->airplay_name_.c_str());
  ESP_LOGCONFIG(TAG, "  buffer_size=%lu", (unsigned long) this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  transport started=%d stream_type=%lld", this->transport_.started(),
                (long long) this->transport_.current_stream_type());
}

void AirPlayReceiver::on_transport_event(TransportEvent event, const TransportEventData *data, void *user_data) {
  AirPlayReceiver *self = static_cast<AirPlayReceiver *>(user_data);
  if (self != nullptr) {
    self->handle_transport_event(event, data);
  }
}

void AirPlayReceiver::handle_transport_event(TransportEvent event, const TransportEventData *data) {
  // Audio engine wire-up point: the transport emits play/pause/volume/metadata
  // and a fully-configured stream (data->audio) here. The decode/output
  // pipeline is not yet ported, so these are logged until wired.
  switch (event) {
    case TRANSPORT_EVENT_AUDIO_CONFIGURED:
      if (data != nullptr) {
        ESP_LOGI(TAG, "transport: audio configured type=%lld data_port=%u control_port=%u event_port=%u "
                      "sample_rate=%d channels=%d shk=%d ekey=%d",
                 (long long) data->audio.stream_type, data->audio.data_port, data->audio.control_port,
                 data->audio.event_port, data->audio.sample_rate, data->audio.channels, (int) data->audio.has_shk,
                 (int) data->audio.has_ekey);
      }
      break;
    case TRANSPORT_EVENT_METADATA:
      if (data != nullptr) {
        ESP_LOGI(TAG, "transport: metadata '%s' / '%s' / '%s'", data->metadata.title, data->metadata.artist,
                 data->metadata.album);
      }
      break;
    case TRANSPORT_EVENT_VOLUME:
      ESP_LOGI(TAG, "transport: volume db=%.2f q15=%d", transport_volume_db(), (int) transport_volume_q15());
      break;
    case TRANSPORT_EVENT_PLAYING:
      ESP_LOGI(TAG, "transport: playing");
      break;
    case TRANSPORT_EVENT_PAUSED:
      ESP_LOGI(TAG, "transport: paused");
      break;
    case TRANSPORT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "transport: disconnected");
      break;
    case TRANSPORT_EVENT_CLIENT_CONNECTED:
      ESP_LOGI(TAG, "transport: client connected");
      break;
    case TRANSPORT_EVENT_ANCHOR:
      if (data != nullptr) {
        ESP_LOGV(TAG, "transport: anchor clock=%llu ns=%llu rtp=%u rate=%.1f",
                 (unsigned long long) data->anchor.clock_id, (unsigned long long) data->anchor.network_time_ns,
                 data->anchor.rtp_time, data->anchor.rate);
      }
      break;
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
