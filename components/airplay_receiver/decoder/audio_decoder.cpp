#include "audio_decoder.h"

#include <netinet/in.h>

#include <cstring>  // memcpy, memset, strcmp, strstr

// Ported from upstream airplay-esp32 main/audio/audio_decoder.c. ALAC and AAC
// come from the esp_audio_codec managed component; raw L16/PCM gets a
// byte-order fixup. All decoding state and scratch are routed through the
// centralized airplay_* allocator (../allocator.h) and all diagnostics through
// esphome/core/log.h so the memory policy lives in one place.
//
// AAC was decoded with Fraunhofer FDK from 2026-10-06 to 2026-10-25, for 5.1;
// see tag parked/surround-5.1 and FIELD-NOTES.md.

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esphome/core/log.h"

#include "../allocator.h"
#include "aac_format.h"
#include "alac_magic_cookie.h"
#include "decoder/impl/esp_aac_dec.h"
#include "decoder/impl/esp_alac_dec.h"
#include "esp_audio_dec.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_receiver.decoder";

#define MAX_FALLBACK_CHANNELS 2

// AAC-LC frame length, and the output delay FDK had. esp_audio_codec emits a
// frame's audio one frame earlier than FDK (measured, tools/aac_bench at
// 9933c7b); holding each frame back by one keeps playout exactly where the FDK
// builds put it, which is the timing multi-room was tested with.
static constexpr size_t AAC_FRAME_SAMPLES = 1024;
static constexpr int AAC_MAX_CHANNELS = 2;
// One INFO line per this many AAC frames (~23 s at 44.1 kHz): decode cost and
// the decode task's remaining stack.
static constexpr uint32_t AAC_STATS_INTERVAL_FRAMES = 1000;

enum audio_decoder_kind_t {
  AUDIO_DECODER_NONE = 0,
  AUDIO_DECODER_PCM,
  AUDIO_DECODER_ALAC,
  AUDIO_DECODER_AAC,
};

struct audio_decoder {
  audio_decoder_kind_t kind;
  audio_format_t format;
  void *alac_decoder;
  void *aac_decoder;
  uint8_t alac_magic_cookie[ALAC_MAGIC_COOKIE_SIZE];
  uint8_t *aac_frame_buffer;
  size_t aac_frame_buffer_size;
  uint32_t aac_ssrc;  // last SSRC seen, to log each format change once
  bool aac_rate_warned;
  uint32_t aac_stats_frames;
  int64_t aac_stats_total_us;
  int64_t aac_stats_max_us;
  int aac_channels;
  // One-frame delay line: `aac_held` is returned on the next decode.
  int16_t *aac_held;
  int16_t *aac_pending;
  size_t aac_held_samples;
};

// Grow-only realloc backed by the airplay_* allocator. We know the previous
// size (`old_size`), so the preserved tail is exactly `old_size` bytes.
static uint8_t *airplay_realloc_grow(uint8_t *ptr, size_t old_size,
                                     size_t new_size) {
  if (new_size <= old_size) {
    return ptr;
  }
  uint8_t *new_ptr = static_cast<uint8_t *>(airplay_alloc(new_size, true));
  if (new_ptr == nullptr) {
    return nullptr;
  }
  if (ptr != nullptr) {
    memcpy(new_ptr, ptr, old_size);
    airplay_free(ptr);
  }
  return new_ptr;
}

static bool codec_is_alac(const char *codec) {
  if (!codec) {
    return false;
  }
  return strcmp(codec, "AppleLossless") == 0 || strcmp(codec, "ALAC") == 0;
}

static bool codec_is_aac(const char *codec) {
  if (!codec) {
    return false;
  }
  return strstr(codec, "AAC") != nullptr || strstr(codec, "aac") != nullptr ||
         strstr(codec, "mpeg4-generic") != nullptr;
}

// The SETUP format, for packets whose SSRC names no AAC format.
static aac_stream_format_t aac_setup_format(const audio_decoder_t *decoder) {
  aac_stream_format_t format{};
  format.sample_rate = decoder->format.sample_rate > 0 ? (uint32_t) decoder->format.sample_rate : 44100;
  format.channel_config = decoder->format.channels == 1 ? 1 : AAC_CHANNEL_CONFIG_STEREO;
  return format;
}

static void aac_log_stats(audio_decoder_t *decoder, int64_t elapsed_us) {
  decoder->aac_stats_frames++;
  decoder->aac_stats_total_us += elapsed_us;
  if (elapsed_us > decoder->aac_stats_max_us) {
    decoder->aac_stats_max_us = elapsed_us;
  }
  if (decoder->aac_stats_frames < AAC_STATS_INTERVAL_FRAMES) {
    return;
  }
  ESP_LOGI(TAG, "aac: %d ch, decode avg=%lld us max=%lld us over %u frames, stack free=%u B, internal free=%u B",
           decoder->aac_channels,
           (long long) (decoder->aac_stats_total_us / decoder->aac_stats_frames),
           (long long) decoder->aac_stats_max_us, (unsigned) decoder->aac_stats_frames,
           (unsigned) uxTaskGetStackHighWaterMark(nullptr), (unsigned) airplay_internal_free());
  decoder->aac_stats_frames = 0;
  decoder->aac_stats_total_us = 0;
  decoder->aac_stats_max_us = 0;
}

audio_decoder_t *audio_decoder_create(const audio_decoder_config_t *config) {
  if (!config) {
    return nullptr;
  }

  // Decoder state is part of the realtime decode path's working set: keep it
  // in internal DRAM (cacheable, fast, no PSRAM flicker).
  audio_decoder_t *decoder =
      static_cast<audio_decoder_t *>(airplay_calloc(1, sizeof(*decoder), true));
  if (!decoder) {
    return nullptr;
  }
  decoder->kind = AUDIO_DECODER_NONE;
  decoder->format = config->format;

  if (codec_is_alac(config->format.codec)) {
    decoder->kind = AUDIO_DECODER_ALAC;
    build_alac_magic_cookie(decoder->alac_magic_cookie, &config->format);

    esp_alac_dec_cfg_t alac_cfg = {};
    alac_cfg.codec_spec_info = decoder->alac_magic_cookie;
    alac_cfg.spec_info_len = ALAC_MAGIC_COOKIE_SIZE;

    esp_audio_err_t err =
        esp_alac_dec_open(&alac_cfg, sizeof(alac_cfg), &decoder->alac_decoder);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGE(TAG, "Failed to open ALAC decoder: %d", err);
      decoder->alac_decoder = nullptr;
      decoder->kind = AUDIO_DECODER_NONE;
    }
  } else if (codec_is_aac(config->format.codec)) {
    decoder->kind = AUDIO_DECODER_AAC;
    // ADTS framing: each packet gets a header built from its SSRC, so the
    // decoder follows rate changes without being reopened.
    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    const size_t frame_bytes = AAC_FRAME_SAMPLES * AAC_MAX_CHANNELS * sizeof(int16_t);
    decoder->aac_held = static_cast<int16_t *>(airplay_calloc(1, frame_bytes, false));
    decoder->aac_pending = static_cast<int16_t *>(airplay_calloc(1, frame_bytes, false));
    // esp_audio_codec takes ~51 KB of internal RAM here.
    const esp_audio_err_t err = esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &decoder->aac_decoder);
    if (err != ESP_AUDIO_ERR_OK || !decoder->aac_held || !decoder->aac_pending) {
      ESP_LOGE(TAG, "Failed to open AAC decoder: %d (internal free=%u B)", err, (unsigned) airplay_internal_free());
      decoder->kind = AUDIO_DECODER_NONE;
    }
  } else if (strcmp(config->format.codec, "L16") == 0 ||
             strcmp(config->format.codec, "PCM") == 0) {
    decoder->kind = AUDIO_DECODER_PCM;
  } else {
    decoder->kind = AUDIO_DECODER_NONE;
  }

  return decoder;
}

void audio_decoder_destroy(audio_decoder_t *decoder) {
  if (!decoder) {
    return;
  }

  if (decoder->alac_decoder) {
    esp_alac_dec_close(decoder->alac_decoder);
    decoder->alac_decoder = nullptr;
  }

  if (decoder->aac_decoder) {
    esp_aac_dec_close(decoder->aac_decoder);
    decoder->aac_decoder = nullptr;
  }
  airplay_free(decoder->aac_held);
  airplay_free(decoder->aac_pending);

  if (decoder->aac_frame_buffer) {
    airplay_free(decoder->aac_frame_buffer);
    decoder->aac_frame_buffer = nullptr;
    decoder->aac_frame_buffer_size = 0;
  }

  airplay_free(decoder);
}

int audio_decoder_decode(audio_decoder_t *decoder, const uint8_t *input,
                         size_t input_len, uint32_t ssrc, int16_t *output,
                         size_t output_capacity_samples,
                         audio_decode_info_t *info) {
  if (!decoder || !input || !output || output_capacity_samples == 0) {
    return -1;
  }

  int channels = decoder->format.channels;
  if (channels <= 0) {
    channels = MAX_FALLBACK_CHANNELS;
  }
  // DEFENSE: the decode scratch buffer is sized for stereo
  // (AUDIO_MAX_CHANNELS 2). A malformed SDP can report a larger channel count
  // that would make frame.len exceed the heap buffer. Clamp here so the ALAC /
  // AAC output length can never overrun the caller's decode_buffer.
  if (channels > MAX_FALLBACK_CHANNELS) {
    channels = MAX_FALLBACK_CHANNELS;
  }

  if (decoder->kind == AUDIO_DECODER_PCM) {
    size_t decoded_samples = input_len / (channels * sizeof(int16_t));
    if (decoded_samples > output_capacity_samples) {
      decoded_samples = output_capacity_samples;
    }

    const int16_t *src = (const int16_t *) input;
    for (size_t i = 0; i < decoded_samples * channels; i++) {
      output[i] = ntohs(src[i]);
    }

    if (info) {
      info->channels = channels;
    }
    return (int) decoded_samples;
  }

  if (decoder->kind == AUDIO_DECODER_ALAC) {
    if (!decoder->alac_decoder) {
      return -1;
    }

    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = (uint8_t *) input;
    raw.len = (uint32_t) input_len;
    raw.consumed = 0;
    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = (uint8_t *) output;
    frame.len = (uint32_t) (output_capacity_samples * channels * sizeof(int16_t));
    frame.decoded_size = 0;

    esp_audio_dec_info_t dec_info = {};

    esp_audio_err_t err =
        esp_alac_dec_decode(decoder->alac_decoder, &raw, &frame, &dec_info);
    if (err != ESP_AUDIO_ERR_OK) {
      return -1;
    }

    int dec_channels = dec_info.channel > 0 ? dec_info.channel : channels;
    if (dec_channels <= 0) {
      dec_channels = MAX_FALLBACK_CHANNELS;
    }

    size_t decoded_samples =
        frame.decoded_size / (dec_channels * sizeof(int16_t));
    if (decoded_samples > output_capacity_samples) {
      decoded_samples = output_capacity_samples;
    }

    if (info) {
      info->channels = dec_channels;
    }
    return (int) decoded_samples;
  }

  if (decoder->kind == AUDIO_DECODER_AAC) {
    if (!decoder->aac_decoder) {
      return -1;
    }

    aac_stream_format_t format{};
    if (!aac_format_from_ssrc(ssrc, &format)) {
      format = aac_setup_format(decoder);
    }
    const uint8_t channel_config = format.channel_config;
    if (ssrc != decoder->aac_ssrc) {
      decoder->aac_ssrc = ssrc;
      ESP_LOGI(TAG, "aac: ssrc=0x%08x -> %u Hz, channel config %u, first bytes %02x %02x %02x %02x",
               (unsigned) ssrc, (unsigned) format.sample_rate, (unsigned) channel_config,
               input_len > 0 ? input[0] : 0, input_len > 1 ? input[1] : 0, input_len > 2 ? input[2] : 0,
               input_len > 3 ? input[3] : 0);
    }
    // The timeline and resampler run at the SETUP rate; a packet at another
    // rate would play at the wrong pitch.
    if (format.sample_rate != (uint32_t) decoder->format.sample_rate && !decoder->aac_rate_warned) {
      decoder->aac_rate_warned = true;
      ESP_LOGW(TAG, "aac: packets are %u Hz but the stream was set up at %d Hz", (unsigned) format.sample_rate,
               decoder->format.sample_rate);
    }

    // AirPlay sends bare raw_data_blocks; the decoder is opened for ADTS, so
    // prepend a header naming this packet's rate and layout.
    const size_t needed = input_len + AAC_ADTS_HEADER_LEN;
    uint8_t *frame_buffer =
        airplay_realloc_grow(decoder->aac_frame_buffer, decoder->aac_frame_buffer_size, needed);
    if (!frame_buffer) {
      return -1;
    }
    decoder->aac_frame_buffer = frame_buffer;
    decoder->aac_frame_buffer_size = needed > decoder->aac_frame_buffer_size ? needed : decoder->aac_frame_buffer_size;
    if (!aac_build_adts_header(frame_buffer, input_len, format.sample_rate, channel_config)) {
      return -1;
    }
    memcpy(frame_buffer + AAC_ADTS_HEADER_LEN, input, input_len);

    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = frame_buffer;
    raw.len = (uint32_t) needed;
    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = reinterpret_cast<uint8_t *>(decoder->aac_pending);
    frame.len = (uint32_t) (AAC_FRAME_SAMPLES * AAC_MAX_CHANNELS * sizeof(int16_t));
    esp_audio_dec_info_t dec_info = {};

    const int64_t started_us = esp_timer_get_time();
    const esp_audio_err_t err = esp_aac_dec_decode(decoder->aac_decoder, &raw, &frame, &dec_info);
    if (err != ESP_AUDIO_ERR_OK) {
      ESP_LOGW(TAG, "AAC decode error %d (ssrc=0x%08x, %u bytes)", err, (unsigned) ssrc, (unsigned) input_len);
      return -1;
    }
    const int dec_channels = dec_info.channel > 0 ? dec_info.channel : channels;
    if (dec_channels > AAC_MAX_CHANNELS) {
      return -1;
    }
    decoder->aac_channels = dec_channels;
    aac_log_stats(decoder, esp_timer_get_time() - started_us);

    const size_t decoded_samples = frame.decoded_size / (dec_channels * sizeof(int16_t));
    // The first frame plays as silence of its own length, as FDK's did.
    const size_t held_samples = decoder->aac_held_samples ? decoder->aac_held_samples : decoded_samples;
    if (held_samples > output_capacity_samples) {
      return -1;
    }
    memcpy(output, decoder->aac_held, held_samples * dec_channels * sizeof(int16_t));
    int16_t *const released = decoder->aac_held;
    decoder->aac_held = decoder->aac_pending;
    decoder->aac_pending = released;
    decoder->aac_held_samples = decoded_samples;

    if (info) {
      info->channels = dec_channels;
    }
    return (int) held_samples;
  }

  return -1;
}

bool audio_decoder_is_aac(const audio_decoder_t *decoder) {
  return decoder && decoder->kind == AUDIO_DECODER_AAC;
}

bool audio_decoder_is_alac(const audio_decoder_t *decoder) {
  return decoder && decoder->kind == AUDIO_DECODER_ALAC;
}

}  // namespace airplay_receiver
}  // namespace esphome
