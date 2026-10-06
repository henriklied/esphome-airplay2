// Host harness for tests/test_aac_fdk.py. Exercises the decoder files exactly
// as they build for the board.
//
//   aac_harness decode <in.adts> <ssrc-hex> <out.pcm>
//       Strip each ADTS header (AirPlay sends bare raw_data_blocks), rebuild it
//       from the SSRC with aac_build_adts_header(), decode to stereo s16le.
//       Prints "frames=<n> source_channels=<n> sample_rate=<n> stack=<bytes>
//       header_mismatches=<n>".
//   aac_harness info <formats-hex> <out.plist>
//       Write the GET /info binary plist.

#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "decoder/aac_fdk.h"
#include "decoder/aac_format.h"
#include "transport/bplist.h"

using namespace esphome::airplay_receiver;

static constexpr size_t DECODE_STACK_BYTES = 256 * 1024;
static constexpr uint8_t STACK_FILL = 0xA5;
static constexpr size_t MAX_FRAMES_PER_PACKET = 2048;
static constexpr int OUTPUT_CHANNELS = 2;

struct DecodeJob {
  std::vector<uint8_t> input;
  uint32_t ssrc;
  FILE *out;
  int frames;
  int source_channels;
  int sample_rate;
  int header_mismatches;
  int failed;
};

static size_t adts_frame_len(const uint8_t *p) {
  return ((size_t) (p[3] & 0x03) << 11) | ((size_t) p[4] << 3) | (p[5] >> 5);
}

static void *decode_thread(void *arg) {
  DecodeJob *job = static_cast<DecodeJob *>(arg);
  aac_stream_format_t format{};
  if (!aac_format_from_ssrc(job->ssrc, &format)) {
    job->failed = 1;
    return nullptr;
  }
  aac_fdk_t *decoder = aac_fdk_open(OUTPUT_CHANNELS);
  if (decoder == nullptr) {
    job->failed = 1;
    return nullptr;
  }
  std::vector<int16_t> pcm(MAX_FRAMES_PER_PACKET * OUTPUT_CHANNELS);
  std::vector<uint8_t> rebuilt;
  size_t pos = 0;
  while (pos + AAC_ADTS_HEADER_LEN <= job->input.size()) {
    const uint8_t *frame = job->input.data() + pos;
    const size_t frame_len = adts_frame_len(frame);
    if (frame_len <= AAC_ADTS_HEADER_LEN || pos + frame_len > job->input.size()) {
      break;
    }
    const size_t payload_len = frame_len - AAC_ADTS_HEADER_LEN;
    rebuilt.resize(frame_len);
    const uint8_t channel_config =
        aac_adts_channel_config(frame + AAC_ADTS_HEADER_LEN, payload_len, format.channel_config);
    if (!aac_build_adts_header(rebuilt.data(), payload_len, format.sample_rate, channel_config)) {
      job->failed = 1;
      break;
    }
    if (memcmp(rebuilt.data(), frame, AAC_ADTS_HEADER_LEN) != 0) {
      job->header_mismatches++;
    }
    memcpy(rebuilt.data() + AAC_ADTS_HEADER_LEN, frame + AAC_ADTS_HEADER_LEN, payload_len);

    aac_fdk_frame_info_t info{};
    const int frames = aac_fdk_decode(decoder, rebuilt.data(), rebuilt.size(), pcm.data(),
                                      MAX_FRAMES_PER_PACKET, &info);
    if (frames < 0) {
      fprintf(stderr, "decode error 0x%x at frame %d\n", info.error, job->frames);
      job->failed = 1;
      break;
    }
    fwrite(pcm.data(), sizeof(int16_t), (size_t) frames * OUTPUT_CHANNELS, job->out);
    job->frames++;
    job->source_channels = info.source_channels;
    job->sample_rate = info.sample_rate;
    pos += frame_len;
  }
  aac_fdk_close(decoder);
  return nullptr;
}

static int run_decode(const char *in_path, const char *ssrc_hex, const char *out_path) {
  DecodeJob job{};
  FILE *in = fopen(in_path, "rb");
  if (in == nullptr) {
    return 2;
  }
  int c;
  while ((c = fgetc(in)) != EOF) {
    job.input.push_back((uint8_t) c);
  }
  fclose(in);
  job.ssrc = (uint32_t) strtoul(ssrc_hex, nullptr, 16);
  job.out = fopen(out_path, "wb");
  if (job.out == nullptr) {
    return 2;
  }

  // Run on a pre-filled stack so the untouched tail measures peak depth.
  void *stack = nullptr;
  if (posix_memalign(&stack, 4096, DECODE_STACK_BYTES) != 0) {
    return 2;
  }
  memset(stack, STACK_FILL, DECODE_STACK_BYTES);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, stack, DECODE_STACK_BYTES);
  pthread_t thread;
  pthread_create(&thread, &attr, decode_thread, &job);
  pthread_join(thread, nullptr);
  fclose(job.out);

  size_t untouched = 0;
  while (untouched < DECODE_STACK_BYTES && static_cast<uint8_t *>(stack)[untouched] == STACK_FILL) {
    untouched++;
  }
  free(stack);
  printf("frames=%d source_channels=%d sample_rate=%d stack=%zu header_mismatches=%d\n", job.frames,
         job.source_channels, job.sample_rate, DECODE_STACK_BYTES - untouched, job.header_mismatches);
  return job.failed ? 1 : 0;
}

static int run_info(const char *formats_hex, const char *out_path) {
  static const uint8_t public_key[32] = {};
  uint8_t body[1024];
  const uint64_t formats = strtoull(formats_hex, nullptr, 16);
  const size_t len = bplist_build_info_response(body, sizeof(body), "AA:BB:CC:DD:EE:FF", "Test", public_key,
                                                sizeof(public_key), 0x1C340405F4A00ULL, 2, formats);
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
  if (argc == 5 && strcmp(argv[1], "decode") == 0) {
    return run_decode(argv[2], argv[3], argv[4]);
  }
  if (argc == 4 && strcmp(argv[1], "info") == 0) {
    return run_info(argv[2], argv[3]);
  }
  fprintf(stderr, "usage: aac_harness decode <in.adts> <ssrc-hex> <out.pcm> | info <formats-hex> <out>\n");
  return 2;
}
