#pragma once
// airplay_receiver audio_packet — metadata carried with one compressed audio
// access unit from ingress through decrypt/decode.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_packet.h (PR #130) into
// the ESPHome component. The upstream layout is preserved 1:1 (field names,
// order and types are unchanged) and the module is wrapped in
// esphome::airplay_receiver.
//
// NOTE: upstream PR #130 ships audio_packet.h only — there is NO
// audio_packet.c. The module is a pure struct definition (immutable access-unit
// metadata); there are no functions, so there is nothing to compile. It lives
// here as a header-only slice for the decode worker / stream tasks to include.
//
// MEMORY POLICY: no heap. `payload` points at a buffer owned and lifetime-managed
// by the caller (a stream/decoder packet buffer); this struct only carries a
// borrowed pointer. Nothing routes through airplay_alloc/airplay_calloc/
// airplay_free.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/* Immutable metadata carried with one compressed audio access unit from
 * ingress through decrypt/decode.  Keeping the epoch beside the RTP timestamp
 * closes the gap where a seek/flush can occur after a packet passes the RTP
 * gate but before its decoder work completes. */
typedef struct {
  uint32_t epoch;
  uint32_t rtp_timestamp;
  const uint8_t *payload;
  size_t payload_len;
  /* Sampled at ingress, where the block counters are coherent: by the time the
   * decode worker reaches this packet the reader has already counted the ones
   * behind it, so the worker cannot re-derive whether this is a priming frame
   * that must be silenced. */
  bool prime_mute;
} audio_encoded_packet_t;

}  // namespace airplay_receiver
}  // namespace esphome
