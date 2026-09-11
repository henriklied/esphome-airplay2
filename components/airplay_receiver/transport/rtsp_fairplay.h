#pragma once
// airplay_receiver FairPlay handshake (port of main/rtsp/rtsp_fairplay.c).
//
// AirPlay 2 senders open the encrypted control channel and immediately POST
// /fp-setup. The exchange is a fixed two-round challenge whose replies are
// constants: round 1 (seq 1) answers with one of four pre-computed 142-byte
// blobs selected by the mode byte, round 2 (seq 3) echoes the last 20 bytes of
// the request behind a fixed 12-byte header. No key material is derived here --
// the audio key still comes from the shk/ekey chain in SETUP -- but a sender
// that is told FairPlay is available (features bits 11/14, raop et=3,5) will
// hang up if /fp-setup is not answered correctly.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

constexpr size_t FP_REPLY_SIZE = 142;
constexpr size_t FP_HEADER_SIZE = 12;
constexpr size_t FP_SETUP2_SUFFIX_LEN = 20;

/**
 * Build the FairPlay response for a POST /fp-setup body.
 *
 * @param body           request body
 * @param body_len       length of body
 * @param response       out: heap buffer (airplay_alloc); caller frees with
 *                       airplay_free on success
 * @param response_len   out: length of the response
 * @return 0 on success, -1 if the request is not a handshake this stage answers
 */
int rtsp_fairplay_handle(const uint8_t *body, size_t body_len, uint8_t **response, size_t *response_len);

}  // namespace airplay_receiver
}  // namespace esphome
