#pragma once
// Stereo pair: one AirPlay destination played by two boards.
//
// The PRIMARY is an ordinary receiver. Every block of PCM that enters its
// timeline is also copied (one channel, or both) into a PSRAM ring that a
// network task drains to the SECONDARY, together with a state snapshot of
// the primary's engine: epoch, format, clock map, play state and volume.
//
// The SECONDARY runs no AirPlay at all -- no mDNS, RTSP or decoder. It
// replays what it is sent into its own engine, so both timelines hold the
// same audio at the same RTP positions under the same clock map, and each
// board's scheduler plays it at the same network time through its own output
// latency and drift servo.
//
// The network time is the secondary's choice (`clock:`):
//   PTP    its own passive PTP listener, pinned to the master the primary's
//          anchor names. Error is the two boards' PTP bias difference.
//   RELAY  the primary's network time, measured over the link by PING/PONG
//          (peer_clock.h). Works when only the primary hears PTP, and also
//          follows the primary's local-clock fallback.
//   AUTO   PTP when it is locked to the right master, else RELAY; sticks to
//          whichever it is using while that stays valid.
//
// The primary's playback never waits on the link: a full ring drops the copy
// and counts it. If the secondary loses the link or its clock it plays
// silence and rejoins at the right sample once both return.
//
// Wire format: peer_protocol.h. Two UDP ports: `port` carries PCM and STATE
// to the secondary, `port + 1` the clock exchange, so a PONG is never queued
// behind audio on either side (queueing before the timestamp is read would
// show up as path asymmetry, i.e. clock error).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace esphome {
namespace airplay_receiver {

enum class PeerRole : uint8_t {
  STANDALONE = 0,
  PRIMARY,
  SECONDARY,
};

/// What the primary forwards. LEFT/RIGHT/MONO send one channel; STEREO sends
/// both, for a secondary whose DSP needs the full mix.
enum class PeerForward : uint8_t {
  LEFT = 0,
  RIGHT,
  MONO,
  STEREO,
};

enum class PeerClockMode : uint8_t {
  AUTO = 0,
  PTP,
  RELAY,
};

struct PeerConfig {
  PeerRole role = PeerRole::STANDALONE;
  /// IPv4 in network byte order. Primary: the secondary to send to
  /// (required). Secondary: the only primary to accept, 0 for any.
  uint32_t peer_ip = 0;
  uint16_t port = 7011;
  PeerForward forward = PeerForward::RIGHT;
  PeerClockMode clock = PeerClockMode::AUTO;
  /// Secondary only: added to its playout time. Positive plays later.
  int32_t delay_us = 0;
};

/// Set before audio_peer_start(); ignored afterwards.
void audio_peer_configure(const PeerConfig &config);

/// Open the sockets and start the role's tasks. Call once the network is up;
/// returns ESP_ERR_INVALID_STATE if already running, ESP_OK for STANDALONE.
esp_err_t audio_peer_start(void);

PeerRole audio_peer_role(void);

// ---------------------------------------------------------------------------
// Primary taps, called by the audio engine. No-ops unless role is PRIMARY.
// ---------------------------------------------------------------------------

/// A block just committed to the primary's timeline. Never blocks.
void audio_peer_tap_pcm(uint32_t epoch, uint32_t first_rtp, const int16_t *pcm, size_t samples, uint8_t channels);

/// The primary's timeline dropped everything from `flush_rtp` on.
void audio_peer_tap_deferred_flush(uint32_t epoch, uint32_t flush_rtp);

// ---------------------------------------------------------------------------
// Secondary
// ---------------------------------------------------------------------------

/**
 * Offset to add to local time to get the network time the primary's anchor
 * lives in, from the clock source in use. Called on every render.
 * @return false when no source is usable: render silence.
 */
bool audio_peer_secondary_offset_ns(int64_t local_ns, int64_t *offset_ns);

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

typedef struct {
  PeerRole role;
  bool link_up;          // secondary: heard from the primary recently
  bool playing;          // secondary: mirrored play state
  bool output_running;   // secondary: mirrored output state
  const char *clock;     // secondary: "ptp", "relay" or "none"
} audio_peer_status_t;

void audio_peer_get_status(audio_peer_status_t *status);

}  // namespace airplay_receiver
}  // namespace esphome
