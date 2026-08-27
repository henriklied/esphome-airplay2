#pragma once
// airplay_receiver RTSP connection state (port of main/rtsp/rtsp_conn.c).
//
// One RtspConn per accepted AirPlay client. Consolidates all per-connection
// session state for the AirPlay 2 control plane: HAP/encryption, stream
// descriptors, volume, and the ports the audio engine will later bind.
//
// The connection owns a CryptoModule HAPSession (paired via create_session)
// and routes all of its own memory through airplay_*.

#include <cstdint>

#include "../crypto/crypto_module.h"

namespace esphome {
namespace airplay_receiver {

// Maximum codec name length stored in a connection.
#define AIRPLAY_CODEC_NAME_MAX 32
// Current volume running default (dB, 0 = max, -30 = mute).
#define AIRPLAY_DEFAULT_VOLUME_DB (-15.0f)

/**
 * Per-connection RTSP/AirPlay session state.
 */
struct RtspConn {
  // HAP / encryption
  HAPSession *hap_session = nullptr;   // owned CryptoModule session (pairing)
  CryptoModule *crypto = nullptr;      // module used to create/free the session
  bool encrypted_mode = false;         // control channel ChaCha20-Poly1305 on

  // Audio streaming state
  volatile bool stream_active = false;
  volatile bool stream_paused = false;
  int64_t stream_type = 96;            // 96 = UDP realtime, 103 = TCP buffered
  uint16_t data_port = 0;              // UDP port for audio data
  uint16_t control_port = 0;           // UDP control (retransmit) port
  uint16_t timing_port = 0;            // timing port
  uint16_t event_port = 0;             // TCP server->client event port
  int event_socket = -1;               // TCP listener fd for the event port
  uint16_t buffered_port = 0;          // TCP port for buffered audio
  uint16_t client_control_port = 0;    // client control port (from SETUP)
  uint16_t client_timing_port = 0;     // client timing port (AirPlay 1)
  uint32_t client_ip = 0;              // client IP (network byte order)

  // Codec info from ANNOUNCE/SETUP
  char codec[AIRPLAY_CODEC_NAME_MAX] = {};
  int sample_rate = 0;
  int channels = 0;
  int bits_per_sample = 0;

  // Volume (Q15 fixed-point: 0 = mute, 32768 = unity)
  volatile int32_t volume_q15 = 16384;
  float volume_db = AIRPLAY_DEFAULT_VOLUME_DB;

  // AirPlay protocol version detected from request shape:
  //   0 = unknown, 1 = classic RAOP, 2 = AirPlay 2
  uint8_t protocol_version = 0;
};

/**
 * Create connection state, allocating a CryptoModule HAP session for pairing.
 * Allocation routes through airplay_calloc.
 *
 * @return New connection, or nullptr on allocation failure.
 */
RtspConn *rtsp_conn_create(CryptoModule *crypto);

/**
 * Free connection state, its HAP session, and any owned resources.
 */
void rtsp_conn_free(RtspConn *conn);

/**
 * Set volume in dB (converts to Q15 internally, emits TRANSPORT_EVENT_VOLUME).
 */
void rtsp_conn_set_volume(RtspConn *conn, float volume_db);

/**
 * Get volume as Q15 scale factor (0 = mute, 32768 = unity).
 */
int32_t rtsp_conn_get_volume_q15(RtspConn *conn);

}  // namespace airplay_receiver
}  // namespace esphome
