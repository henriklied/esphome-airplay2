// A/B render of realtime packet loss: plays a 44.1 kHz 16-bit stereo WAV
// through the production timeline with holes punched in the pattern measured
// on Geneva, and writes two files:
//   <out>/plc.wav      concealment as shipped (audio/audio_plc.cpp)
//   <out>/silence.wav  the previous behaviour: 2 ms fade to silence and back
// Driven by tools/plc_demo/run.py.

#include "audio/audio_timeline.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace esphome::airplay_receiver;

extern "C" void *airplay_alloc(size_t bytes, bool) { return std::malloc(bytes); }
extern "C" void *airplay_calloc(size_t count, size_t bytes, bool) {
  return std::calloc(count, bytes);
}
extern "C" void airplay_free(void *memory) { std::free(memory); }

static constexpr uint32_t PACKET = 352;
static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr uint32_t EPOCH = 1;
static constexpr uint16_t CAPACITY = 600;
/* Packets inserted ahead of playout, ~2 s as on the board. */
static constexpr uint32_t LOOKAHEAD = 250;
/* Burst sizes seen on 2026-11-20, one every 4 s. */
static constexpr uint32_t BURSTS[] = {2, 6, 17, 28, 40};
static constexpr uint32_t BURST_SPACING = 4 * SAMPLE_RATE / PACKET;
static constexpr uint32_t OLD_FADE_FRAMES = 88;

static std::vector<int16_t> read_wav(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    std::exit(1);
  }
  std::vector<uint8_t> bytes;
  uint8_t buffer[65536];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) {
    bytes.insert(bytes.end(), buffer, buffer + n);
  }
  std::fclose(f);
  auto u16 = [&](size_t at) { return uint32_t(bytes[at] | bytes[at + 1] << 8); };
  auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0) {
    std::fprintf(stderr, "not a RIFF file\n");
    std::exit(1);
  }
  for (size_t at = 12; at + 8 <= bytes.size();) {
    const uint32_t size = u32(at + 4);
    if (std::memcmp(&bytes[at], "fmt ", 4) == 0) {
      if (u16(at + 8) != 1 || u16(at + 10) != 2 || u32(at + 12) != SAMPLE_RATE ||
          u16(at + 22) != 16) {
        std::fprintf(stderr, "need PCM 16-bit stereo 44.1 kHz\n");
        std::exit(1);
      }
    } else if (std::memcmp(&bytes[at], "data", 4) == 0) {
      const size_t samples = std::min<size_t>(size, bytes.size() - at - 8) / 2;
      std::vector<int16_t> pcm(samples);
      std::memcpy(pcm.data(), &bytes[at + 8], samples * 2);
      return pcm;
    }
    at += 8 + size + (size & 1U);
  }
  std::fprintf(stderr, "no data chunk\n");
  std::exit(1);
}

static void write_wav(const std::string &path, const std::vector<int16_t> &pcm) {
  FILE *f = std::fopen(path.c_str(), "wb");
  auto u16 = [&](uint32_t v) { std::fputc(v & 0xFF, f); std::fputc(v >> 8 & 0xFF, f); };
  auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
  const uint32_t data = uint32_t(pcm.size() * 2);
  std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(2); u32(SAMPLE_RATE); u32(SAMPLE_RATE * 4); u16(4); u16(16);
  std::fwrite("data", 1, 4, f); u32(data);
  std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
}

static std::set<uint32_t> lost_packets(uint32_t packets) {
  std::set<uint32_t> lost;
  for (uint32_t i = 0, start = BURST_SPACING; start < packets;
       i++, start += BURST_SPACING) {
    const uint32_t burst = BURSTS[i % (sizeof BURSTS / sizeof BURSTS[0])];
    for (uint32_t p = start; p < start + burst && p < packets; p++) {
      lost.insert(p);
    }
  }
  return lost;
}

static std::vector<int16_t> render_plc(const std::vector<int16_t> &in,
                                       uint32_t packets,
                                       const std::set<uint32_t> &lost) {
  audio_timeline_t timeline{};
  if (audio_timeline_init(&timeline, CAPACITY, PACKET) != ESP_OK) {
    std::exit(1);
  }
  std::vector<int16_t> out(size_t(packets) * PACKET * 2);
  uint32_t inserted = 0;
  for (uint32_t p = 0; p < packets; p++) {
    for (; inserted < packets && inserted <= p + LOOKAHEAD; inserted++) {
      if (lost.count(inserted)) {
        continue;
      }
      audio_timeline_reservation_t reservation{};
      int16_t *slot = nullptr;
      audio_timeline_reserve(&timeline, EPOCH, inserted * PACKET, &reservation,
                             &slot);
      std::memcpy(slot, &in[size_t(inserted) * PACKET * 2], PACKET * 4);
      audio_timeline_commit(&timeline, &reservation);
    }
    size_t concealed = 0;
    audio_timeline_read(&timeline, EPOCH, p * PACKET, &out[size_t(p) * PACKET * 2],
                        PACKET, 2, true, &concealed);
  }
  std::printf("plc: two-sided %u, forward-only %u\n",
              timeline.plc.two_sided_events, timeline.plc.forward_events);
  audio_timeline_deinit(&timeline);
  return out;
}

static std::vector<int16_t> render_silence(const std::vector<int16_t> &in,
                                           uint32_t packets,
                                           const std::set<uint32_t> &lost) {
  std::vector<int16_t> out(in.begin(), in.begin() + size_t(packets) * PACKET * 2);
  for (uint32_t p : lost) {
    std::memset(&out[size_t(p) * PACKET * 2], 0, PACKET * 4);
  }
  for (uint32_t p : lost) {
    const bool starts = !lost.count(p - 1), ends = !lost.count(p + 1);
    for (uint32_t i = 0; i < OLD_FADE_FRAMES; i++) {
      const float gain = 1.0f - float(i + 1) / OLD_FADE_FRAMES;
      if (starts && p > 0) {
        for (int ch = 0; ch < 2; ch++) {
          const size_t at = size_t(p) * PACKET * 2 + i * 2 + ch;
          out[at] = int16_t(out[size_t(p) * PACKET * 2 - 2 + ch] * gain);
        }
      }
      if (ends && p + 1 < packets) {
        for (int ch = 0; ch < 2; ch++) {
          const size_t at = size_t(p + 1) * PACKET * 2 + i * 2 + ch;
          out[at] = int16_t(in[at] * (1.0f - gain));
        }
      }
    }
  }
  return out;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: plc_demo <in.wav> <out-dir>\n");
    return 2;
  }
  const auto in = read_wav(argv[1]);
  const uint32_t packets = uint32_t(in.size() / 2 / PACKET);
  const auto lost = lost_packets(packets);
  std::printf("%u packets, %zu lost\n", packets, lost.size());
  write_wav(std::string(argv[2]) + "/plc.wav", render_plc(in, packets, lost));
  write_wav(std::string(argv[2]) + "/silence.wav", render_silence(in, packets, lost));
  return 0;
}
