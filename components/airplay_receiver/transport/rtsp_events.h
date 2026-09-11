#pragma once
// airplay_receiver RTSP event system (port of main/rtsp/rtsp_events.c).
//
// Observer pattern for playback/control state changes. The audio engine (and
// the media_player consumer) register a single callback to react to RTSP
// control-flow events without coupling to the transport. Events carry enough
// metadata for the audio engine to configure a stream (codec, ports, crypto)
// and to expose play/pause volume state to Home Assistant.
//
// All event emissions are safe to call from the RTSP client/server tasks.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

enum TransportEvent : uint8_t {
  TRANSPORT_EVENT_CLIENT_CONNECTED = 0,
  TRANSPORT_EVENT_PLAYING = 1,
  TRANSPORT_EVENT_PAUSED = 2,
  TRANSPORT_EVENT_DISCONNECTED = 3,
  TRANSPORT_EVENT_METADATA = 4,
  // Transport-specific, used to hand a fully-configured stream to the audio
  // engine. Emitted on the AirPlay 2 stream SETUP path so the audio engine can
  // open the data/control/event sockets and start decoding.
  TRANSPORT_EVENT_AUDIO_CONFIGURED = 5,
  TRANSPORT_EVENT_VOLUME = 6,
  // Playout anchor (SETRATEANCHORTIME) — real-time stream alignment.
  TRANSPORT_EVENT_ANCHOR = 7,
  // Stream flush (FLUSH / FLUSHBUFFERED). Payload = TransportFlush.
  TRANSPORT_EVENT_FLUSH = 8,
};

#define TRANSPORT_METADATA_STRING_MAX 64

/// Track metadata carried by TRANSPORT_EVENT_METADATA.
struct TransportMetadata {
  char title[TRANSPORT_METADATA_STRING_MAX] = {};
  char artist[TRANSPORT_METADATA_STRING_MAX] = {};
  char album[TRANSPORT_METADATA_STRING_MAX] = {};
  char genre[TRANSPORT_METADATA_STRING_MAX] = {};
  uint32_t duration_secs = 0;
  uint32_t position_secs = 0;
  bool has_artwork = false;
};

/// Audio stream description handed to the engine via TRANSPORT_EVENT_AUDIO_CONFIGURED.
struct TransportAudioConfig {
  int64_t stream_type = 96;       // 96 = UDP realtime, 103 = TCP buffered
  uint16_t data_port = 0;         // port the audio engine should listen on
  uint16_t control_port = 0;      // control (NACK/retransmit) port
  uint16_t event_port = 0;        // server->client event port (AirPlay 2)
  uint16_t timing_port = 0;       // timing port (AirPlay 1)
  uint16_t buffered_port = 0;     // TCP port for buffered audio (type 103)
  // Where to send NACK resend requests. The engine arms retransmission only
  // when both are non-zero; without them a lost RTP packet is simply concealed.
  uint32_t client_ip = 0;             // sender address, network byte order
  uint16_t client_control_port = 0;   // sender's control port
  uint32_t audio_buffer_size = 0; // advertised AP2_AUDIO_BUFFER_SIZE
  int sample_rate = 0;
  int channels = 0;
  int bits_per_sample = 0;
  int64_t codec_type = 0;         // bplist "ct": 2=ALAC, 4=AAC, 8=AAC-ELD
  int frame_size = 0;             // bplist "spf": samples per frame (ALAC 352, AAC 1024)
  uint32_t playout_latency_samples = 0;  // bplist "latencyMin" (11025 = 250ms realtime; 0 buffered)
  // ChaCha20-Poly1305 audio key material (AirPlay 2 only). `shk` (per-stream
  // shared secret) is preferred; the engine falls back to ekey/session after.
  bool has_shk = false;
  uint8_t shk[32] = {};
  size_t shk_len = 0;
  bool has_ekey = false;
  uint8_t ekey[64] = {};
  size_t ekey_len = 0;
  bool has_eiv = false;
  uint8_t eiv[16] = {};
  size_t eiv_len = 0;
  // Fully-resolved stream encryption key (transport runs the shk/ekey/derive
  // chain through CryptoModule::configure_audio_encryption before emitting).
  bool has_encrypt = false;
  uint8_t encrypt_key[32] = {};
  size_t encrypt_key_len = 0;
};

/// Playout anchor carried by TRANSPORT_EVENT_ANCHOR (SETRATEANCHORTIME).
struct TransportAnchor {
  uint64_t clock_id = 0;        // networkTimeTimelineID
  uint64_t network_time_ns = 0; // anchor in PTP nanosecond time
  uint32_t rtp_time = 0;        // matching RTP timestamp
  double rate = 0.0;            // 1.0 = play, 0.0 = paused
};

/// Stream flush carried by TRANSPORT_EVENT_FLUSH (FLUSH / FLUSHBUFFERED).
struct TransportFlush {
  // RTP timestamp to flush up to (deferred flush, FLUSHBUFFERED with
  // flushUntilTS); 0 = immediate seek-flush (FLUSH).
  uint32_t flush_until_ts = 0;
};

union TransportEventData {
  TransportMetadata metadata;     // TRANSPORT_EVENT_METADATA
  TransportAudioConfig audio;     // TRANSPORT_EVENT_AUDIO_CONFIGURED
  TransportAnchor anchor;         // TRANSPORT_EVENT_ANCHOR
  TransportFlush flush;           // TRANSPORT_EVENT_FLUSH
};

/// Event callback invoked from RTSP tasks. `data` is valid for the duration of
/// the call and may be null for events without payload.
typedef void (*TransportEventCallback)(TransportEvent event,
                                       const TransportEventData *data,
                                       void *user_data);

/// Register a single listener (last one registered wins; audio engine wires
/// this once at setup()). Returns 0 on success.
int transport_events_register(TransportEventCallback callback, void *user_data);

/// Emit an event to the registered listener. Safe from any task.
void transport_events_emit(TransportEvent event, const TransportEventData *data);

/// Current stream/volume state exposed as accessors for the media_player.
float transport_volume_db();
int32_t transport_volume_q15();
int64_t transport_stream_type();

/// Mirror the current volume state (called by the RTSP handlers as volume
/// changes arrive, so the accessors above stay current from any task).
void transport_set_volume_state(float volume_db, int32_t volume_q15);
/// Mirror the current stream type (set by SETUP/RECORD/flush handlers).
void transport_set_stream_type(int64_t stream_type);

}  // namespace airplay_receiver
}  // namespace esphome
