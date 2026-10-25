// Benchmark only, never ship: stereo AAC decode cost, FDK against
// esp_audio_codec, on the audio core. Decides whether FDK can go once 5.1 does.
#include "aac_bench.h"

#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "decoder/impl/esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esphome/components/airplay_receiver/decoder/aac_fdk.h"
#include "esphome/core/log.h"

#include "clips.h"

namespace esphome {
namespace aac_bench {

static const char *const TAG = "aac_bench";

struct Clip {
  const char *name;
  const uint8_t *data;
  size_t len;
};
static const Clip CLIPS[] = {
    {"stereo 44.1k 256k (aac_at)", clip_s441_at, clip_s441_at_len},
    {"stereo 48k 256k (aac_at)", clip_s48_at, clip_s48_at_len},
    {"stereo 44.1k 256k (ffmpeg)", clip_s441_ff, clip_s441_ff_len},
};

enum Decoder { FDK, ESP, DECODERS };
static const char *const DECODER_NAMES[DECODERS] = {"fdk", "esp_audio_codec"};

static constexpr int PASSES = 3;
static constexpr int CHANNELS = 2;
static constexpr size_t AAC_FRAME_SAMPLES = 1024;
static constexpr size_t OUT_FRAMES = 2048;
// FDK: the stack audio_decode has today (audio_decode_worker.cpp). esp_audio_codec
// ran in 6 KB before FDK; 16 KB leaves room to measure.
static constexpr uint32_t DECODE_STACK_BYTES[DECODERS] = {57344, 16384};
static constexpr UBaseType_t DECODE_PRIORITY = 6;
static constexpr BaseType_t AUDIO_CORE = 1;
// esp_audio_codec and FDK may start output at different points in the stream.
static constexpr int MAX_LAG_SAMPLES = 3 * AAC_FRAME_SAMPLES;
static constexpr size_t LAG_WINDOW_SAMPLES = 2048;
static constexpr uint32_t ADTS_HEADER_BYTES = 7;
// Per channel; the clips are 3 s.
static constexpr size_t CAPTURE_SAMPLES = 4 * 48000;
static constexpr uint32_t BOOT_DELAY_MS = 20000;
static constexpr uint32_t ROUND_INTERVAL_MS = 30000;
// IDLE frees a deleted task's stack; wait for it so heap deltas are clean.
static constexpr uint32_t TASK_REAP_MS = 100;

struct Run {
  const Clip *clip;
  Decoder decoder;
  TaskHandle_t caller;
  int16_t *capture;  // first pass, interleaved; PSRAM
  size_t capture_capacity;
  size_t captured;   // samples per channel
};

/// Opens, decodes and closes one decoder. Returns frames decoded into `out`, -1 on error.
class Session {
 public:
  explicit Session(Decoder decoder) : decoder_(decoder) {
    if (decoder == FDK) {
      this->fdk_ = airplay_receiver::aac_fdk_open(CHANNELS);
      this->ok_ = this->fdk_ != nullptr;
    } else {
      esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
      this->ok_ = esp_aac_dec_open(&cfg, sizeof(cfg), &this->esp_) == ESP_AUDIO_ERR_OK;
    }
  }
  ~Session() {
    if (this->fdk_ != nullptr) airplay_receiver::aac_fdk_close(this->fdk_);
    if (this->esp_ != nullptr) esp_aac_dec_close(this->esp_);
  }
  bool ok() const { return this->ok_; }

  int decode(const uint8_t *adts, size_t len, int16_t *out) {
    if (this->decoder_ == FDK) {
      airplay_receiver::aac_fdk_frame_info_t info;
      return airplay_receiver::aac_fdk_decode(this->fdk_, adts, len, out, OUT_FRAMES, &info);
    }
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(adts);
    raw.len = len;
    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = reinterpret_cast<uint8_t *>(out);
    frame.len = OUT_FRAMES * CHANNELS * sizeof(int16_t);
    esp_audio_dec_info_t info = {};
    if (esp_aac_dec_decode(this->esp_, &raw, &frame, &info) != ESP_AUDIO_ERR_OK) return -1;
    return frame.decoded_size / (CHANNELS * sizeof(int16_t));
  }

 private:
  Decoder decoder_;
  airplay_receiver::aac_fdk_t *fdk_ = nullptr;
  void *esp_ = nullptr;
  bool ok_ = false;
};

static size_t free_internal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static size_t free_psram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
static size_t largest_internal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

static void decode_task(void *arg) {
  Run &run = *static_cast<Run *>(arg);
  static int16_t out[OUT_FRAMES * CHANNELS];
  const size_t internal_before = free_internal(), psram_before = free_psram(), largest_before = largest_internal();
  {
    Session session(run.decoder);
    const size_t internal_open = internal_before - free_internal(), psram_open = psram_before - free_psram();
    if (!session.ok()) {
      ESP_LOGE(TAG, "%s: open failed, internal free=%u largest=%u", DECODER_NAMES[run.decoder],
               (unsigned) internal_before, (unsigned) largest_before);
    } else {
      int64_t total_us = 0, max_us = 0;
      uint32_t frames = 0, errors = 0;
      bool warmed_up = false;
      for (int pass = 0; pass < PASSES; pass++) {
        size_t pos = 0;
        while (pos + ADTS_HEADER_BYTES <= run.clip->len) {
          const uint8_t *h = run.clip->data + pos;
          const size_t len = ((h[3] & 0x03) << 11) | (h[4] << 3) | (h[5] >> 5);
          if (len < ADTS_HEADER_BYTES || pos + len > run.clip->len) break;
          const int64_t t = esp_timer_get_time();
          const int n = session.decode(h, len, out);
          const int64_t dt = esp_timer_get_time() - t;
          pos += len;
          // Let IDLE1 feed the task watchdog; the real decoder blocks on its queue between frames.
          vTaskDelay(1);
          if (n < 0) {
            errors++;
            continue;
          }
          if (pass == 0 && run.captured + n <= run.capture_capacity) {
            memcpy(run.capture + run.captured * CHANNELS, out, n * CHANNELS * sizeof(int16_t));
            run.captured += n;
          }
          if (!warmed_up) {
            // The first frame carries the decoder's configuration: not timed.
            warmed_up = true;
            continue;
          }
          total_us += dt;
          if (dt > max_us) max_us = dt;
          frames++;
        }
      }
      const uint32_t stack_used = DECODE_STACK_BYTES[run.decoder] - uxTaskGetStackHighWaterMark(nullptr);
      if (frames == 0) {
        ESP_LOGE(TAG, "%s %s: no frames (errors=%u)", run.clip->name, DECODER_NAMES[run.decoder], (unsigned) errors);
      } else {
        ESP_LOGI(TAG, "%s %-15s: frames=%u err=%u avg=%.2f ms max=%.2f ms stack=%u heap: internal=%u psram=%u",
                 run.clip->name, DECODER_NAMES[run.decoder], (unsigned) frames, (unsigned) errors,
                 total_us / 1000.0f / frames, max_us / 1000.0f, (unsigned) stack_used, (unsigned) internal_open,
                 (unsigned) psram_open);
      }
    }
  }
  xTaskNotifyGive(run.caller);
  vTaskDelete(nullptr);
}

/// Sample offset of `test` against `ref` (left channel) with the highest correlation.
static int best_lag(const int16_t *ref, size_t ref_n, const int16_t *test, size_t test_n) {
  int best = 0;
  double best_corr = -1e300;
  const size_t start = MAX_LAG_SAMPLES;
  for (int lag = -MAX_LAG_SAMPLES; lag <= MAX_LAG_SAMPLES; lag++) {
    double corr = 0;
    for (size_t i = start; i < start + LAG_WINDOW_SAMPLES; i++) {
      const long j = (long) i + lag;
      if (i >= ref_n || j < 0 || (size_t) j >= test_n) break;
      corr += (double) ref[i * CHANNELS] * test[j * CHANNELS];
    }
    if (corr > best_corr) best_corr = corr, best = lag;
    if ((lag & 0xff) == 0) vTaskDelay(1);
  }
  return best;
}

/// Signal-to-difference ratio of `test` against `ref`, both channels, after aligning by `lag`.
static float snr_db(const int16_t *ref, size_t ref_n, const int16_t *test, size_t test_n, int lag) {
  double signal = 0, noise = 0;
  for (size_t i = 0; i < ref_n; i++) {
    const long j = (long) i + lag;
    if (j < 0) continue;
    if ((size_t) j >= test_n) break;
    for (int c = 0; c < CHANNELS; c++) {
      const double r = ref[i * CHANNELS + c], d = r - test[j * CHANNELS + c];
      signal += r * r, noise += d * d;
    }
  }
  return noise == 0 ? INFINITY : 10.0f * log10f((float) (signal / noise));
}

static void bench_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(BOOT_DELAY_MS));
  for (int round = 1;; round++) {
    ESP_LOGI(TAG, "round %d, internal free=%u psram free=%u", round, (unsigned) free_internal(),
             (unsigned) free_psram());
    for (const Clip &clip : CLIPS) {
      Run runs[DECODERS];
      for (int d = 0; d < DECODERS; d++) {
        runs[d] = {&clip, (Decoder) d, xTaskGetCurrentTaskHandle(), nullptr, CAPTURE_SAMPLES, 0};
        runs[d].capture = static_cast<int16_t *>(
            heap_caps_malloc(runs[d].capture_capacity * CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM));
        if (runs[d].capture == nullptr) runs[d].capture_capacity = 0;
        // A fresh task per decoder, so each stack high-water mark is its own.
        xTaskCreatePinnedToCore(decode_task, "aac_bench_dec", DECODE_STACK_BYTES[d], &runs[d], DECODE_PRIORITY,
                                nullptr, AUDIO_CORE);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(TASK_REAP_MS));
      }
      const Run &ref = runs[FDK], &test = runs[ESP];
      if (ref.captured && test.captured) {
        const int lag = best_lag(ref.capture, ref.captured, test.capture, test.captured);
        ESP_LOGI(TAG, "  %s vs fdk: lag=%d samples, snr=%.1f dB (samples %u vs %u)", DECODER_NAMES[ESP], lag,
                 snr_db(ref.capture, ref.captured, test.capture, test.captured, lag), (unsigned) test.captured,
                 (unsigned) ref.captured);
      }
      for (Run &run : runs) heap_caps_free(run.capture);
    }
    vTaskDelay(pdMS_TO_TICKS(ROUND_INTERVAL_MS));
  }
}

void AacBench::setup() { xTaskCreatePinnedToCore(bench_task, "aac_bench", 8192, nullptr, 5, nullptr, 0); }

}  // namespace aac_bench
}  // namespace esphome
