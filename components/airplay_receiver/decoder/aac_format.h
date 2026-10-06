#pragma once
// AAC stream format signalling for AirPlay 2 buffered audio.
//
// A buffered (type 103) AAC packet is a bare raw_data_block: no ADTS header,
// no AudioSpecificConfig. The sender names the format twice -- once in the
// SETUP "audioFormat" bitmask, and again on every packet in the RTP SSRC field,
// which can change mid-stream. Values and approach follow shairport-sync 5.0
// (player.h ssrc_t, ap2_buffered_audio_processor.c), which prepends an ADTS
// header built from the SSRC so a stock AAC decoder learns the channel layout.
//
// No ESPHome or ESP-IDF dependencies: tests/test_aac_fdk.py compiles this file
// on the host.

#include <stddef.h>
#include <stdint.h>

namespace esphome {
namespace airplay_receiver {

static constexpr size_t AAC_ADTS_HEADER_LEN = 7;

/// RTP SSRC values seen on buffered AirPlay 2 AAC packets.
enum aac_ssrc_t : uint32_t {
  AAC_SSRC_44100_STEREO = 0x16000000,
  AAC_SSRC_48000_STEREO = 0x17000000,
  AAC_SSRC_48000_5POINT1 = 0x27000000,
  AAC_SSRC_48000_7POINT1 = 0x28000000,  // Dolby Atmos arrives as this
};

/// SETUP "audioFormat" / GET /info "supportedFormats.bufferStream" bits.
static constexpr uint64_t AIRPLAY_FORMAT_AAC_LC_44100_STEREO = 1ULL << 22;
static constexpr uint64_t AIRPLAY_FORMAT_AAC_LC_48000_STEREO = 1ULL << 23;
static constexpr uint64_t AIRPLAY_FORMAT_AAC_LC_48000_5POINT1 = 1ULL << 39;
static constexpr uint64_t AIRPLAY_FORMAT_AAC_LC_48000_7POINT1 = 1ULL << 40;

/// MPEG-4 channelConfiguration values (ISO/IEC 14496-3 table 1.19).
static constexpr uint8_t AAC_CHANNEL_CONFIG_STEREO = 2;
static constexpr uint8_t AAC_CHANNEL_CONFIG_5POINT1 = 6;
static constexpr uint8_t AAC_CHANNEL_CONFIG_7POINT1 = 7;

typedef struct {
  uint32_t sample_rate;
  uint8_t channel_config;
} aac_stream_format_t;

/// Format named by a packet's RTP SSRC. False for anything that is not a known
/// AAC SSRC (ALAC, realtime streams, zero).
bool aac_format_from_ssrc(uint32_t ssrc, aac_stream_format_t *out);

/// Format named by a SETUP "audioFormat" value. False unless exactly one known
/// AAC-LC bit is set.
bool aac_format_from_audio_format(uint64_t audio_format, aac_stream_format_t *out);

/// channelConfiguration to put in the ADTS header for `payload`. A
/// raw_data_block that opens with a program_config_element describes its own
/// layout and needs channelConfiguration 0 -- Apple's AudioToolbox encodes 7.1
/// that way -- otherwise the SSRC's configuration applies.
uint8_t aac_adts_channel_config(const uint8_t *payload, size_t payload_len, uint8_t ssrc_channel_config);

/// Number of PCM channels a channelConfiguration carries (0 if unknown).
int aac_channel_count(uint8_t channel_config);

/// Write a 7-byte ADTS header for an AAC-LC raw_data_block of `payload_len`
/// bytes. Returns false for a sample rate ADTS cannot signal or a frame too
/// long for its 13-bit length field.
bool aac_build_adts_header(uint8_t header[AAC_ADTS_HEADER_LEN], size_t payload_len,
                           uint32_t sample_rate, uint8_t channel_config);

}  // namespace airplay_receiver
}  // namespace esphome
