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

// Memory placement for FDK's section-tagged allocations.
//
// The pschatzmann FDK port puts every allocation in PSRAM. FDK itself tags its
// per-frame work buffers with a memory section (SECT_DATA_L1 = fastest, L2,
// EXTERN) and leaves placement to the platform; upstream ignores the tag.
// Wrapping FDKcalloc_L / FDKaalloc_L at link time (-Wl,--wrap, set in
// __init__.py) honours it without patching the pinned component: L1 goes to
// internal RAM while enough stays free for WiFi and lwIP, everything else
// stays in PSRAM. Stereo AAC-LC tags 2 x 8 KB as L1 and 53 KB as L2 (measured
// on the host); L2 does not fit beside WiFi, so only L1 moves.
//
// FDKafree_L / FDKfree_L are not wrapped: they read the pointer stored below
// the aligned block and call free(), which accepts either heap. These live in
// this file, not their own, because the linker only pulls an archive member
// that something already references; aac_fdk_open() guarantees this one.

#ifdef USE_ESP_IDF

#include "esp_heap_caps.h"

#include "genericStds.h"

namespace {

// Internal RAM left free after an L1 allocation. ~60 KB is free while
// streaming with FDK open; the headroom is for WiFi RX buffers and sockets.
constexpr size_t INTERNAL_RESERVE_BYTES = 40 * 1024;

bool is_fast_section(MEMORY_SECTION section) {
  return section == SECT_DATA_L1 || section == SECT_DATA_L1_A || section == SECT_DATA_L1_B;
}

void *placed_calloc(size_t bytes, MEMORY_SECTION section) {
  if (is_fast_section(section) &&
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + INTERNAL_RESERVE_BYTES) {
    void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ptr != nullptr) {
      return ptr;
    }
  }
  void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
  return ptr != nullptr ? ptr : heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
}

}  // namespace

extern "C" {

void *__wrap_FDKcalloc_L(const UINT dim, const UINT size, MEMORY_SECTION section) {
  return placed_calloc((size_t) dim * size, section);
}

// Same layout as the original, so the unwrapped FDKafree_L can release it.
void *__wrap_FDKaalloc_L(const UINT size, const UINT alignment, MEMORY_SECTION section) {
  void *addr = placed_calloc((size_t) size + alignment + sizeof(void *), section);
  if (addr == nullptr) {
    return nullptr;
  }
  void *result = ALIGN_PTR((unsigned char *) addr + sizeof(void *));
  *(((void **) result) - 1) = addr;
  return result;
}

}  // extern "C"

#endif  // USE_ESP_IDF
