// airplay_receiver RTSP connection state (port of main/rtsp/rtsp_conn.c).
#include "rtsp_conn.h"

#include "../allocator.h"
#include "rtsp_events.h"
#include "socket_utils.h"
#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_conn";

// Convert dB (0 = max, -30 = mute) into a perceptual Q15 multiplier using the
// upstream squared volume curve.
static int32_t db_to_q15(float volume_db) {
  if (volume_db <= -30.0f) {
    return 0;
  }
  if (volume_db >= 0.0f) {
    return 32768;
  }
  float normalized = (volume_db + 30.0f) / 30.0f;
  float curved = normalized * normalized;
  return static_cast<int32_t>(curved * 32768.0f);
}

RtspConn *rtsp_conn_create(CryptoModule *crypto) {
  RtspConn *conn = static_cast<RtspConn *>(airplay_calloc(1, sizeof(RtspConn), false));
  if (conn == nullptr) {
    return nullptr;
  }

  conn->crypto = crypto;
  conn->volume_db = AIRPLAY_DEFAULT_VOLUME_DB;
  conn->volume_q15 = db_to_q15(conn->volume_db);
  // Publish the default immediately. The audio engine's gain is unity until
  // something sets it, and clients send SET_PARAMETER volume only *after* the
  // stream is already running -- so without this the first moments of every
  // connection play at full scale before dropping to the requested level.
  transport_set_volume_state(conn->volume_db, conn->volume_q15);
  transport_events_emit(TRANSPORT_EVENT_VOLUME, nullptr);

  if (crypto != nullptr) {
    conn->hap_session = crypto->create_session();
    if (conn->hap_session == nullptr) {
      ESP_LOGW(TAG, "Failed to create CryptoModule HAP session");
      // Connection is still usable (pairing will retry), but it cannot pair.
    }
  }

  return conn;
}

void rtsp_conn_free(RtspConn *conn) {
  if (conn == nullptr) {
    return;
  }
  if (conn->hap_session != nullptr && conn->crypto != nullptr) {
    conn->crypto->free_session(conn->hap_session);
    conn->hap_session = nullptr;
  }
  // Release any stream-port reservations that were not consumed by the audio
  // engine (e.g. a timing port that is never bound, or a SETUP that never got
  // a RECORD). Consumed ports are no longer registered, so this is a no-op.
  socket_utils_release_reservation(conn->data_port);
  socket_utils_release_reservation(conn->control_port);
  socket_utils_release_reservation(conn->timing_port);
  socket_utils_release_reservation(conn->buffered_port);
  conn->crypto = nullptr;
  airplay_free(conn);
}

void rtsp_conn_set_volume(RtspConn *conn, float volume_db) {
  if (conn == nullptr) {
    return;
  }
  conn->volume_db = volume_db;
  conn->volume_q15 = db_to_q15(volume_db);
  transport_set_volume_state(conn->volume_db, conn->volume_q15);
  transport_events_emit(TRANSPORT_EVENT_VOLUME, nullptr);
}

int32_t rtsp_conn_get_volume_q15(RtspConn *conn) {
  if (conn == nullptr) {
    return 32768;
  }
  return conn->volume_q15;
}

}  // namespace airplay_receiver
}  // namespace esphome
