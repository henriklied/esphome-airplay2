#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/* About 2 ms at the native AirPlay rates. State belongs to the PCM consumer
 * and survives render boundaries, including the servo's one-frame reads. */
struct audio_gap_fader_t {
  static constexpr uint32_t FADE_FRAMES = 88;
  int16_t last[2];
  int16_t fade_origin[2];
  uint32_t fade_position;
  bool missing;

  void reset() {
    last[0] = last[1] = 0;
    fade_origin[0] = fade_origin[1] = 0;
    fade_position = FADE_FRAMES;
    missing = false;
  }

  void conceal(int16_t *out, size_t frames, uint8_t channels) {
    if (!missing) {
      missing = true;
      fade_position = 0;
      for (uint8_t ch = 0; ch < channels; ch++) {
        fade_origin[ch] = last[ch];
      }
    }
    for (size_t i = 0; i < frames; i++) {
      const int32_t gain = FADE_FRAMES - fade_position;
      for (uint8_t ch = 0; ch < channels; ch++) {
        last[ch] = static_cast<int16_t>(
            static_cast<int32_t>(fade_origin[ch]) * gain /
            static_cast<int32_t>(FADE_FRAMES));
        out[i * channels + ch] = last[ch];
      }
      if (fade_position < FADE_FRAMES) {
        fade_position++;
      }
    }
  }

  void recover(int16_t *pcm, size_t frames, uint8_t channels) {
    if (frames == 0) {
      return;
    }
    if (missing) {
      missing = false;
      fade_position = 0;
      for (uint8_t ch = 0; ch < channels; ch++) {
        fade_origin[ch] = last[ch];
      }
    }
    size_t i = 0;
    while (i < frames && fade_position < FADE_FRAMES) {
      fade_position++;
      for (uint8_t ch = 0; ch < channels; ch++) {
        const int32_t prior = static_cast<int32_t>(fade_origin[ch]) *
                              static_cast<int32_t>(FADE_FRAMES - fade_position);
        const int32_t current = static_cast<int32_t>(pcm[i * channels + ch]) *
                                static_cast<int32_t>(fade_position);
        pcm[i * channels + ch] =
            static_cast<int16_t>((prior + current) /
                                 static_cast<int32_t>(FADE_FRAMES));
      }
      i++;
    }
    for (uint8_t ch = 0; ch < channels; ch++) {
      last[ch] = pcm[(frames - 1) * channels + ch];
    }
  }
};

}  // namespace airplay_receiver
}  // namespace esphome
