// airplay_receiver audio_buffer — decoder scratch space.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_buffer.c (PR #130) into the
// ESPHome component. The upstream logic is preserved exactly; the differences
// are:
//   * everything is wrapped in esphome::airplay_receiver;
//   * logging uses esphome/core/log.h (ESP_LOGE) with a file-scope TAG;
//   * heap routes through airplay_alloc / airplay_free (../allocator.h) rather
//     than malloc/free.
//
// MEMORY POLICY (justified in the matching header): the decode scratch buffer
// is the per-frame decode working set, small and cacheable, so it stays in
// internal DRAM -> airplay_alloc(size, realtime=true), matching upstream's
// malloc() placement.

#include "audio_buffer.h"

#include "esphome/core/log.h"

#include "../allocator.h"

#include <cstring>

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "audio_buf";

/* ---------- init / deinit ---------- */

esp_err_t audio_buffer_init(audio_buffer_t *buffer) {
  if (!buffer) {
    return ESP_ERR_INVALID_ARG;
  }

  std::memset(buffer, 0, sizeof(*buffer));

  const size_t capacity_samples =
      (size_t) MAX_SAMPLES_PER_FRAME * AUDIO_MAX_CHANNELS;
  buffer->decode_buffer = static_cast<int16_t *>(airplay_alloc(
      capacity_samples * sizeof(int16_t), /* realtime=*/true));
  if (!buffer->decode_buffer) {
    ESP_LOGE(TAG, "Failed to allocate decode buffer");
    return ESP_ERR_NO_MEM;
  }
  buffer->decode_capacity_samples = MAX_SAMPLES_PER_FRAME;

  return ESP_OK;
}

void audio_buffer_deinit(audio_buffer_t *buffer) {
  if (!buffer) {
    return;
  }

  airplay_free(buffer->decode_buffer);
  buffer->decode_buffer = nullptr;
  buffer->decode_capacity_samples = 0;
}

/* ---------- decode buffer accessor ---------- */

int16_t *audio_buffer_get_decode_buffer(audio_buffer_t *buffer,
                                        size_t *capacity_samples) {
  if (!buffer) {
    return nullptr;
  }

  if (capacity_samples) {
    *capacity_samples = buffer->decode_capacity_samples;
  }
  return buffer->decode_buffer;
}

}  // namespace airplay_receiver
}  // namespace esphome
