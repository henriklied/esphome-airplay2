// airplay_receiver RTSP event system (port of main/rtsp/rtsp_events.c).
#include "rtsp_events.h"

#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_events";

// Static state lives in the transport, so a single listener is enough: the
// audio engine registers once at setup(). Kept plain-C so it can be called
// from any RTSP task without locking.
static TransportEventCallback s_callback = nullptr;
static void *s_user_data = nullptr;

// Last-known stream/volume state (mirrors the active connection). The audio
// engine / media_player reads these on request.
static float s_volume_db = -15.0f;
static int32_t s_volume_q15 = 16384;
static int64_t s_stream_type = 96;

int transport_events_register(TransportEventCallback callback, void *user_data) {
  s_callback = callback;
  s_user_data = user_data;
  ESP_LOGI(TAG, "Registered RTSP transport event listener");
  return 0;
}

void transport_events_emit(TransportEvent event, const TransportEventData *data) {
  // Keep mirrored state up to date so accessors are always current.
  if (data != nullptr) {
    if (event == TRANSPORT_EVENT_VOLUME) {
      // volume_db/q15 are set by the transport directly via the setter helpers
      // below; nothing to do from the payload here.
    } else if (event == TRANSPORT_EVENT_AUDIO_CONFIGURED) {
      s_stream_type = data->audio.stream_type;
    }
  }
  if (s_callback != nullptr) {
    s_callback(event, data, s_user_data);
  }
}

// Set/read the mirrored volume + stream type. These are invoked by the RTSP
// handlers (rtsp_conn_set_volume, SETUP/RECORD) which run in the client task.
void transport_set_volume_state(float volume_db, int32_t volume_q15) {
  s_volume_db = volume_db;
  s_volume_q15 = volume_q15;
}

void transport_set_stream_type(int64_t stream_type) { s_stream_type = stream_type; }

float transport_volume_db() { return s_volume_db; }

int32_t transport_volume_q15() { return s_volume_q15; }

int64_t transport_stream_type() { return s_stream_type; }

}  // namespace airplay_receiver
}  // namespace esphome
