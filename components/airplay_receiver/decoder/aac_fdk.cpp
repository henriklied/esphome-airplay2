#include "aac_fdk.h"

#include <new>

#include "aacdecoder_lib.h"

namespace esphome {
namespace airplay_receiver {

struct aac_fdk {
  HANDLE_AACDECODER handle;
  int output_channels;
  bool clear_history;
};

aac_fdk_t *aac_fdk_open(int output_channels) {
  aac_fdk_t *decoder = new (std::nothrow) aac_fdk_t{};
  if (decoder == nullptr) {
    return nullptr;
  }
  decoder->handle = aacDecoder_Open(TT_MP4_ADTS, 1);
  if (decoder->handle == nullptr) {
    delete decoder;
    return nullptr;
  }
  decoder->output_channels = output_channels;
  // Min = max pins the output: mono is duplicated up, 5.1 / 7.1 mixed down.
  aacDecoder_SetParam(decoder->handle, AAC_PCM_MIN_OUTPUT_CHANNELS, output_channels);
  aacDecoder_SetParam(decoder->handle, AAC_PCM_MAX_OUTPUT_CHANNELS, output_channels);
  // The limiter delays output by its lookahead and costs CPU; the board's DSP
  // already owns headroom (see warn_if_dsp_clips_), so keep the decoder
  // sample-aligned with the RTP timestamps the scheduler plays against.
  aacDecoder_SetParam(decoder->handle, AAC_PCM_LIMITER_ENABLE, 0);
  return decoder;
}

void aac_fdk_close(aac_fdk_t *decoder) {
  if (decoder == nullptr) {
    return;
  }
  aacDecoder_Close(decoder->handle);
  delete decoder;
}

int aac_fdk_decode(aac_fdk_t *decoder, const uint8_t *adts_frame, size_t adts_len, int16_t *out,
                   size_t out_capacity_frames, aac_fdk_frame_info_t *info) {
  *info = {};
  UCHAR *buffers[] = {const_cast<UCHAR *>(adts_frame)};
  const UINT sizes[] = {(UINT) adts_len};
  UINT bytes_valid = (UINT) adts_len;

  AAC_DECODER_ERROR err = aacDecoder_Fill(decoder->handle, buffers, sizes, &bytes_valid);
  if (err == AAC_DEC_OK) {
    const UINT flags = decoder->clear_history ? AACDEC_CLRHIST : 0;
    err = aacDecoder_DecodeFrame(decoder->handle, out,
                                 (INT) (out_capacity_frames * (size_t) decoder->output_channels), flags);
  }
  if (err != AAC_DEC_OK) {
    // Drop whatever is left of the failed frame so the next one starts clean.
    aacDecoder_SetParam(decoder->handle, AAC_TPDEC_CLEAR_BUFFER, 1);
    decoder->clear_history = true;
    info->error = (int) err;
    return -1;
  }
  decoder->clear_history = false;

  const CStreamInfo *stream = aacDecoder_GetStreamInfo(decoder->handle);
  info->sample_rate = stream->sampleRate;
  info->source_channels = stream->aacNumChannels;
  return stream->frameSize;
}

}  // namespace airplay_receiver
}  // namespace esphome
