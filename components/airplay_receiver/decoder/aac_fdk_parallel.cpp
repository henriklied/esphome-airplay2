#include "aac_fdk_parallel.h"

#ifdef USE_ESP_IDF

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "aacdecoder.h"
#include "block.h"
#include "esphome/core/log.h"

// Why the deferred channels stay bit-identical (FDK at the pinned ref):
//
// - Spectra live in per-channel slices of workBufferCore2 and the overlap state
//   in each channel's static info; nothing the main core touches later in the
//   frame. The one shared buffer, pWorkBufferCore1->mdctOutTemp, is reached
//   through pComStaticData, so the worker swaps in a struct pointing at its own.
// - Right after the filterbank, the loop calls CConcealment_TDFading on the
//   same samples. With the concealment state Ok, previously Ok, and fade_old at
//   full scale, every fading station is full scale and TDNoise adds nothing, so
//   it reads and writes no samples; it only advances the noise seed, which it
//   still does on this core in order. Any other state runs serially.
// - Nothing after the loop in CAacDecoder_DecodeFrame reads the time signal
//   (USAC pseudo-LR aside, which AirPlay never sends), and its wrapper waits
//   for the worker before returning, including on error paths.

namespace esphome {
namespace airplay_receiver {
namespace {

const char *const TAG = "aac_fdk_parallel";

// Filterbank only: measured peak ~6.3 KB on 5.1 (high-water mark logged on stop).
constexpr uint32_t WORKER_STACK = 10240;
// Same as audio_decode; the other core, which it is not pinned to.
constexpr UBaseType_t WORKER_PRIORITY = 6;
constexpr BaseType_t WORKER_CORE = 0;
// FDK decodes at most 8 channels; at most half of them are deferred.
constexpr UBaseType_t MAX_JOBS = 8;

struct FilterbankJob {
  CAacDecoderStaticChannelInfo *static_info;
  CAacDecoderChannelInfo *channel_info;
  PCM_DEC *out;
  SHORT frame_len;
  int frame_ok;
  INT headroom;
  UINT el_flags;
  INT el_ch;
};

QueueHandle_t jobs = nullptr;
SemaphoreHandle_t done = nullptr;
TaskHandle_t worker = nullptr;
CWorkBufferCore1 *worker_scratch = nullptr;
CAacDecoderCommonStaticData worker_static;
bool enabled = false;
int frame_calls = 0;
int pending = 0;

bool concealment_is_steady(const CAacDecoderStaticChannelInfo *static_info) {
  const CConcealmentInfo &info = static_info->concealmentInfo;
  return info.concealState == ConcealState_Ok && info.concealState_old == ConcealState_Ok &&
         info.fade_old == (FIXP_DBL) MAXVAL_DBL;
}

}  // namespace
}  // namespace airplay_receiver
}  // namespace esphome

using esphome::airplay_receiver::FilterbankJob;

// Mangled names, matching the -Wl,--wrap list in __init__.py.
void real_frequency_to_time(CAacDecoderStaticChannelInfo *, CAacDecoderChannelInfo *, PCM_DEC[], const SHORT,
                            const int, FIXP_DBL *, const INT, UINT, INT)
    __asm__("__real__Z22CBlock_FrequencyToTimeP28CAacDecoderStaticChannelInfoP22CAacDecoderChannelInfoPlsiS3_iji");
void wrap_frequency_to_time(CAacDecoderStaticChannelInfo *, CAacDecoderChannelInfo *, PCM_DEC[], const SHORT,
                            const int, FIXP_DBL *, const INT, UINT, INT)
    __asm__("__wrap__Z22CBlock_FrequencyToTimeP28CAacDecoderStaticChannelInfoP22CAacDecoderChannelInfoPlsiS3_iji");
AAC_DECODER_ERROR real_decode_frame(HANDLE_AACDECODER, const UINT, PCM_DEC *, const INT, const int)
    __asm__("__real__Z23CAacDecoder_DecodeFrameP20AAC_DECODER_INSTANCEjPlii");
AAC_DECODER_ERROR wrap_decode_frame(HANDLE_AACDECODER, const UINT, PCM_DEC *, const INT, const int)
    __asm__("__wrap__Z23CAacDecoder_DecodeFrameP20AAC_DECODER_INSTANCEjPlii");

void wrap_frequency_to_time(CAacDecoderStaticChannelInfo *static_info, CAacDecoderChannelInfo *channel_info,
                            PCM_DEC out[], const SHORT frame_len, const int frame_ok, FIXP_DBL *work,
                            const INT headroom, UINT el_flags, INT el_ch) {
  using namespace esphome::airplay_receiver;
  // Even calls go to the worker, so it starts on the first channel while this
  // core takes the second; it runs a little slower on core 0, next to WiFi.
  const bool defer = enabled && frame_ok && (frame_calls++ & 1) == 0 && concealment_is_steady(static_info);
  if (defer) {
    const FilterbankJob job{static_info, channel_info, out, frame_len, frame_ok, headroom, el_flags, el_ch};
    if (xQueueSend(jobs, &job, 0) == pdTRUE) {
      pending++;
      return;
    }
  }
  real_frequency_to_time(static_info, channel_info, out, frame_len, frame_ok, work, headroom, el_flags, el_ch);
}

AAC_DECODER_ERROR wrap_decode_frame(HANDLE_AACDECODER self, const UINT flags, PCM_DEC *time_data,
                                    const INT time_data_size, const int channel_offset) {
  using namespace esphome::airplay_receiver;
  frame_calls = 0;
  const AAC_DECODER_ERROR err = real_decode_frame(self, flags, time_data, time_data_size, channel_offset);
  for (; pending > 0; pending--) {
    xSemaphoreTake(done, portMAX_DELAY);
  }
  return err;
}

namespace esphome {
namespace airplay_receiver {
namespace {

void worker_task(void *) {
  FilterbankJob job;
  for (;;) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    CAacDecoderCommonStaticData *shared = job.channel_info->pComStaticData;
    worker_static = *shared;
    worker_static.pWorkBufferCore1 = worker_scratch;
    job.channel_info->pComStaticData = &worker_static;
    real_frequency_to_time(job.static_info, job.channel_info, job.out, job.frame_len, job.frame_ok,
                           worker_scratch->mdctOutTemp, job.headroom, job.el_flags, job.el_ch);
    job.channel_info->pComStaticData = shared;
    xSemaphoreGive(done);
  }
}

}  // namespace

bool aac_fdk_parallel_start() {
  if (worker != nullptr) {
    return true;
  }
  // Through the FDKcalloc_L wrapper, so it is placed like FDK's own L1 buffer.
  worker_scratch = static_cast<CWorkBufferCore1 *>(FDKcalloc_L(1, sizeof(CWorkBufferCore1), SECT_DATA_L1));
  jobs = xQueueCreate(MAX_JOBS, sizeof(FilterbankJob));
  done = xSemaphoreCreateCounting(MAX_JOBS, 0);
  if (worker_scratch == nullptr || jobs == nullptr || done == nullptr ||
      xTaskCreatePinnedToCore(worker_task, "aac_filterbank", WORKER_STACK, nullptr, WORKER_PRIORITY, &worker,
                              WORKER_CORE) != pdPASS) {
    ESP_LOGW(TAG, "Worker unavailable, decoding on one core");
    aac_fdk_parallel_stop();
    return false;
  }
  return true;
}

void aac_fdk_parallel_stop() {
  enabled = false;
  if (worker != nullptr) {
    ESP_LOGI(TAG, "Worker stopped, stack free=%u", (unsigned) uxTaskGetStackHighWaterMark(worker));
    vTaskDelete(worker);
    worker = nullptr;
  }
  if (jobs != nullptr) {
    vQueueDelete(jobs);
    jobs = nullptr;
  }
  if (done != nullptr) {
    vSemaphoreDelete(done);
    done = nullptr;
  }
  if (worker_scratch != nullptr) {
    FDKfree_L(worker_scratch);
    worker_scratch = nullptr;
  }
}

void aac_fdk_parallel_enable(bool on) { enabled = on && worker != nullptr; }

}  // namespace airplay_receiver
}  // namespace esphome

#else  // host build

namespace esphome {
namespace airplay_receiver {

bool aac_fdk_parallel_start() { return false; }
void aac_fdk_parallel_stop() {}
void aac_fdk_parallel_enable(bool) {}

}  // namespace airplay_receiver
}  // namespace esphome

#endif  // USE_ESP_IDF
