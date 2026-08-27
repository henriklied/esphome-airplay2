#pragma once
// airplay_receiver ALAC magic cookie (ALACSpecificConfig) builder.
//
// Ported from upstream airplay-esp32 main/alac_magic_cookie.{c,h}. Builds the
// 24-byte Apple Lossless magic cookie (big-endian ALACSpecificConfig) that the
// esp_audio_codec ALAC decoder needs as its codec_spec_info. Pure value
// serialization -- no heap, no logging.

#include <stdint.h>

#include "audio_decoder.h"  // for airplay_receiver::audio_format_t

#define ALAC_MAGIC_COOKIE_SIZE 24

namespace esphome {
namespace airplay_receiver {

/// Serialize the ALAC-specific config of `fmt` into the 24-byte `cookie`
/// buffer (ALACSpecificConfig, big-endian). Uses sensible defaults for any
/// zeroed field, matching the upstream caller.
void build_alac_magic_cookie(uint8_t *cookie, const audio_format_t *fmt);

}  // namespace airplay_receiver
}  // namespace esphome
