#pragma once
// Fraunhofer FDK AAC decoder, configured for AirPlay: ADTS input (see
// aac_format.h for the header built from the RTP SSRC), any MPEG-4 channel
// configuration up to 7.1 in, a fixed interleaved channel count out. A
// multichannel stream is mixed down inside FDK (ISO/IEC 14496-3 downmix,
// stream metadata when present), so everything after the decoder stays stereo.
//
// No ESPHome dependencies: tests/test_aac_fdk.py compiles this on the host.

#include <stddef.h>
#include <stdint.h>

namespace esphome {
namespace airplay_receiver {

typedef struct aac_fdk aac_fdk_t;

typedef struct {
  int sample_rate;
  int source_channels;  // channels in the bitstream, before the downmix
  int error;            // AAC_DECODER_ERROR of the failing call, 0 on success
} aac_fdk_frame_info_t;

/// Open a decoder producing `output_channels` interleaved channels (1 or 2).
/// Returns nullptr if FDK cannot allocate its state.
aac_fdk_t *aac_fdk_open(int output_channels);

void aac_fdk_close(aac_fdk_t *decoder);

/// Decode one complete ADTS frame into `out` (interleaved 16-bit PCM holding
/// up to `out_capacity_frames` frames). Returns frames written, or -1 on error
/// with `info->error` set. After an error the next call clears the decoder's
/// history, so a corrupt frame costs one frame rather than a stuck decoder.
int aac_fdk_decode(aac_fdk_t *decoder, const uint8_t *adts_frame, size_t adts_len, int16_t *out,
                   size_t out_capacity_frames, aac_fdk_frame_info_t *info);

}  // namespace airplay_receiver
}  // namespace esphome
