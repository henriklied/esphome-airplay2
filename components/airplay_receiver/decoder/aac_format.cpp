#include "aac_format.h"

namespace esphome {
namespace airplay_receiver {

static constexpr uint8_t ADTS_PROFILE_AAC_LC = 1;  // audio object type 2, minus one
static constexpr size_t ADTS_MAX_FRAME_LEN = 0x1FFF;
static constexpr uint8_t AAC_ID_PCE = 5;  // syntactic element id, ISO/IEC 14496-3 table 4.85
static constexpr uint8_t AAC_CHANNEL_CONFIG_IN_PCE = 0;
static constexpr uint32_t ADTS_SAMPLE_RATES[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                 22050, 16000, 12000, 11025, 8000,  7350};

bool aac_format_from_ssrc(uint32_t ssrc, aac_stream_format_t *out) {
  switch (ssrc) {
    case AAC_SSRC_44100_STEREO:
      *out = {44100, AAC_CHANNEL_CONFIG_STEREO};
      return true;
    case AAC_SSRC_48000_STEREO:
      *out = {48000, AAC_CHANNEL_CONFIG_STEREO};
      return true;
    case AAC_SSRC_48000_5POINT1:
      *out = {48000, AAC_CHANNEL_CONFIG_5POINT1};
      return true;
    case AAC_SSRC_48000_7POINT1:
      *out = {48000, AAC_CHANNEL_CONFIG_7POINT1};
      return true;
    default:
      return false;
  }
}

bool aac_format_from_audio_format(uint64_t audio_format, aac_stream_format_t *out) {
  switch (audio_format) {
    case AIRPLAY_FORMAT_AAC_LC_44100_STEREO:
      *out = {44100, AAC_CHANNEL_CONFIG_STEREO};
      return true;
    case AIRPLAY_FORMAT_AAC_LC_48000_STEREO:
      *out = {48000, AAC_CHANNEL_CONFIG_STEREO};
      return true;
    case AIRPLAY_FORMAT_AAC_LC_48000_5POINT1:
      *out = {48000, AAC_CHANNEL_CONFIG_5POINT1};
      return true;
    case AIRPLAY_FORMAT_AAC_LC_48000_7POINT1:
      *out = {48000, AAC_CHANNEL_CONFIG_7POINT1};
      return true;
    default:
      return false;
  }
}

uint8_t aac_adts_channel_config(const uint8_t *payload, size_t payload_len, uint8_t ssrc_channel_config) {
  if (payload_len > 0 && (payload[0] >> 5) == AAC_ID_PCE) {
    return AAC_CHANNEL_CONFIG_IN_PCE;
  }
  return ssrc_channel_config;
}

int aac_channel_count(uint8_t channel_config) {
  switch (channel_config) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      return channel_config;
    case AAC_CHANNEL_CONFIG_7POINT1:
      return 8;
    default:
      return 0;
  }
}

bool aac_build_adts_header(uint8_t header[AAC_ADTS_HEADER_LEN], size_t payload_len,
                           uint32_t sample_rate, uint8_t channel_config) {
  int freq_index = -1;
  for (size_t i = 0; i < sizeof(ADTS_SAMPLE_RATES) / sizeof(ADTS_SAMPLE_RATES[0]); i++) {
    if (ADTS_SAMPLE_RATES[i] == sample_rate) {
      freq_index = (int) i;
      break;
    }
  }
  const size_t frame_len = payload_len + AAC_ADTS_HEADER_LEN;
  if (freq_index < 0 || frame_len > ADTS_MAX_FRAME_LEN || channel_config > 7) {
    return false;
  }

  header[0] = 0xFF;
  header[1] = 0xF1;  // MPEG-4, layer 0, no CRC
  header[2] = (uint8_t) ((ADTS_PROFILE_AAC_LC << 6) | (freq_index << 2) | (channel_config >> 2));
  header[3] = (uint8_t) (((channel_config & 3) << 6) | (frame_len >> 11));
  header[4] = (uint8_t) ((frame_len >> 3) & 0xFF);
  header[5] = (uint8_t) (((frame_len & 7) << 5) | 0x1F);  // buffer fullness 0x7FF (VBR)
  header[6] = 0xFC;
  return true;
}

}  // namespace airplay_receiver
}  // namespace esphome
