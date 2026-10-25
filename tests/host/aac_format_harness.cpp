// Host harness for tests/test_aac_format.py. Exercises decoder/aac_format.cpp
// and transport/bplist.cpp exactly as they build for the board.
//
//   aac_format_harness headers <in.adts> <ssrc-hex>
//       Rebuild every ADTS header from the SSRC with aac_build_adts_header(),
//       as the firmware does for AirPlay's bare raw_data_blocks, and compare it
//       with the encoder's own. Prints "frames=<n> header_mismatches=<n>".
//   aac_format_harness info <formats-hex> [<audio-stream-hex>] <out.plist>
//       Write the GET /info binary plist.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "decoder/aac_format.h"
#include "transport/bplist.h"

using namespace esphome::airplay_receiver;

static size_t adts_frame_len(const uint8_t *p) {
  return ((size_t) (p[3] & 0x03) << 11) | ((size_t) p[4] << 3) | (p[5] >> 5);
}

static int run_headers(const char *in_path, const char *ssrc_hex) {
  FILE *in = fopen(in_path, "rb");
  if (in == nullptr) {
    return 2;
  }
  std::vector<uint8_t> input;
  int c;
  while ((c = fgetc(in)) != EOF) {
    input.push_back((uint8_t) c);
  }
  fclose(in);

  aac_stream_format_t format{};
  if (!aac_format_from_ssrc((uint32_t) strtoul(ssrc_hex, nullptr, 16), &format)) {
    fprintf(stderr, "unknown ssrc %s\n", ssrc_hex);
    return 1;
  }
  int frames = 0, mismatches = 0;
  size_t pos = 0;
  while (pos + AAC_ADTS_HEADER_LEN <= input.size()) {
    const uint8_t *frame = input.data() + pos;
    const size_t frame_len = adts_frame_len(frame);
    if (frame_len <= AAC_ADTS_HEADER_LEN || pos + frame_len > input.size()) {
      break;
    }
    uint8_t rebuilt[AAC_ADTS_HEADER_LEN];
    if (!aac_build_adts_header(rebuilt, frame_len - AAC_ADTS_HEADER_LEN, format.sample_rate,
                               format.channel_config)) {
      return 1;
    }
    if (memcmp(rebuilt, frame, AAC_ADTS_HEADER_LEN) != 0) {
      mismatches++;
    }
    frames++;
    pos += frame_len;
  }
  printf("frames=%d header_mismatches=%d\n", frames, mismatches);
  return 0;
}

static int run_info(const char *formats_hex, const char *audio_hex, const char *out_path) {
  static const uint8_t public_key[32] = {};
  uint8_t body[1024];
  const uint64_t formats = strtoull(formats_hex, nullptr, 16);
  const uint64_t audio_formats = strtoull(audio_hex, nullptr, 16);
  const size_t len = bplist_build_info_response(body, sizeof(body), "AA:BB:CC:DD:EE:FF", "Test", public_key,
                                                sizeof(public_key), 0x1C340405F4A00ULL, 2, formats,
                                                audio_formats);
  if (len == 0) {
    return 1;
  }
  FILE *out = fopen(out_path, "wb");
  if (out == nullptr) {
    return 2;
  }
  fwrite(body, 1, len, out);
  fclose(out);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "headers") == 0) {
    return run_headers(argv[2], argv[3]);
  }
  if (argc == 4 && strcmp(argv[1], "info") == 0) {
    return run_info(argv[2], "0", argv[3]);
  }
  if (argc == 5 && strcmp(argv[1], "info") == 0) {
    return run_info(argv[2], argv[3], argv[4]);
  }
  fprintf(stderr, "usage: aac_format_harness headers <in.adts> <ssrc-hex> | info <formats-hex> [<audio-hex>] <out>\n");
  return 2;
}
