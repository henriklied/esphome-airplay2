#pragma once
// airplay_receiver audio decoder (ALAC / AAC / raw PCM -> PCM).
//
// Ported from upstream airplay-esp32 main/audio/audio_decoder.{c,h}. The
// audio_format_t, audio_decoder_config_t and audio_decode_info_t types keep
// the upstream signatures so the rest of the component can drive the decoder
// exactly as the upstream audio_receiver / audio_stream code does. This is an
// isolated C++ module: every allocation is routed through the centralized
// airplay_* allocator (see ../allocator.h) and all diagnostics go through
// esphome/core/log.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../audio/audio_receiver.h"  // audio_format_t (single source of truth)

namespace esphome {
namespace airplay_receiver {

/// Opaque decoder handle. Defined privately in audio_decoder.cpp.
typedef struct audio_decoder audio_decoder_t;

/// Configuration passed to audio_decoder_create().
typedef struct {
  audio_format_t format;
} audio_decoder_config_t;

/// Per-frame decode result metadata.
typedef struct {
  int channels;
} audio_decode_info_t;

/// Create a decoder for the codec named in `config->format.codec`.
///
/// Returns an opaque handle, or NULL on allocation / open failure. The handle
/// must be released with audio_decoder_destroy(). All decode state is
/// allocated through the airplay_* allocator.
audio_decoder_t *audio_decoder_create(const audio_decoder_config_t *config);

/// Destroy a decoder previously returned by audio_decoder_create().
void audio_decoder_destroy(audio_decoder_t *decoder);

/// Decode one frame of `input` (input_len bytes) into `output` interleaved
/// 16-bit PCM. `output_capacity_samples` is the maximum number of samples
/// (per channel) the caller can hold. Returns the number of decoded samples
/// (per channel) on success, or -1 on error / unsupported codec.
int audio_decoder_decode(audio_decoder_t *decoder, const uint8_t *input,
                         size_t input_len, int16_t *output,
                         size_t output_capacity_samples,
                         audio_decode_info_t *info);

/// True if the decoder was created for an AAC codec.
bool audio_decoder_is_aac(const audio_decoder_t *decoder);

/// True if the decoder was created for an ALAC codec.
bool audio_decoder_is_alac(const audio_decoder_t *decoder);

}  // namespace airplay_receiver
}  // namespace esphome
