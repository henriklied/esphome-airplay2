// Benchmark only, never ship: per-phase FDK decode timing on the audio core.
#include "fdk_bench.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/components/airplay_receiver/decoder/aac_fdk.h"
#include "esphome/core/log.h"

#include "block.h"
#include "channel.h"
#include "pcmdmx_lib.h"

#include "clips.h"

namespace {

enum Phase { READ, TOOLS, IMDCT, DMX, PHASES };
const char *const PHASE_NAMES[PHASES] = {"read", "tools", "imdct", "dmx"};
int64_t g_phase_us[PHASES];
uint32_t g_imdct_calls;

}  // namespace

// --- link-time wrappers (names set by -Wl,--wrap in __init__.py) ---
AAC_DECODER_ERROR real_read(HANDLE_FDK_BITSTREAM, CAacDecoderChannelInfo *[], CAacDecoderStaticChannelInfo *[],
                            const AUDIO_OBJECT_TYPE, SamplingRateInfo *, const UINT, const UINT, const UINT,
                            const UCHAR, const SCHAR, HANDLE_TRANSPORTDEC)
    __asm__("__real__Z20CChannelElement_ReadP13FDK_BITSTREAMPP22CAacDecoderChannelInfoPP28CAacDecoderStaticChannelInfo17AUDIO_OBJECT_TYPEP16SamplingRateInfojjjhaP12TRANSPORTDEC");
AAC_DECODER_ERROR wrap_read(HANDLE_FDK_BITSTREAM, CAacDecoderChannelInfo *[], CAacDecoderStaticChannelInfo *[],
                            const AUDIO_OBJECT_TYPE, SamplingRateInfo *, const UINT, const UINT, const UINT,
                            const UCHAR, const SCHAR, HANDLE_TRANSPORTDEC)
    __asm__("__wrap__Z20CChannelElement_ReadP13FDK_BITSTREAMPP22CAacDecoderChannelInfoPP28CAacDecoderStaticChannelInfo17AUDIO_OBJECT_TYPEP16SamplingRateInfojjjhaP12TRANSPORTDEC");
AAC_DECODER_ERROR wrap_read(HANDLE_FDK_BITSTREAM bs, CAacDecoderChannelInfo *ci[], CAacDecoderStaticChannelInfo *sci[],
                            const AUDIO_OBJECT_TYPE aot, SamplingRateInfo *sri, const UINT flags, const UINT el_flags,
                            const UINT frame_length, const UCHAR channels, const SCHAR ep, HANDLE_TRANSPORTDEC tp) {
  int64_t t = esp_timer_get_time();
  AAC_DECODER_ERROR e = real_read(bs, ci, sci, aot, sri, flags, el_flags, frame_length, channels, ep, tp);
  g_phase_us[READ] += esp_timer_get_time() - t;
  return e;
}

void real_tools(CAacDecoderChannelInfo *[2], CAacDecoderStaticChannelInfo *[2], SamplingRateInfo *, UINT, UINT, int)
    __asm__("__real__Z22CChannelElement_DecodePP22CAacDecoderChannelInfoPP28CAacDecoderStaticChannelInfoP16SamplingRateInfojji");
void wrap_tools(CAacDecoderChannelInfo *[2], CAacDecoderStaticChannelInfo *[2], SamplingRateInfo *, UINT, UINT, int)
    __asm__("__wrap__Z22CChannelElement_DecodePP22CAacDecoderChannelInfoPP28CAacDecoderStaticChannelInfoP16SamplingRateInfojji");
void wrap_tools(CAacDecoderChannelInfo *ci[2], CAacDecoderStaticChannelInfo *sci[2], SamplingRateInfo *sri, UINT flags,
                UINT el_flags, int el_channels) {
  int64_t t = esp_timer_get_time();
  real_tools(ci, sci, sri, flags, el_flags, el_channels);
  g_phase_us[TOOLS] += esp_timer_get_time() - t;
}

void real_imdct(CAacDecoderStaticChannelInfo *, CAacDecoderChannelInfo *, PCM_DEC[], const SHORT, const int,
                FIXP_DBL *, const INT, UINT, INT)
    __asm__("__real__Z22CBlock_FrequencyToTimeP28CAacDecoderStaticChannelInfoP22CAacDecoderChannelInfoPlsiS3_iji");
void wrap_imdct(CAacDecoderStaticChannelInfo *, CAacDecoderChannelInfo *, PCM_DEC[], const SHORT, const int,
                FIXP_DBL *, const INT, UINT, INT)
    __asm__("__wrap__Z22CBlock_FrequencyToTimeP28CAacDecoderStaticChannelInfoP22CAacDecoderChannelInfoPlsiS3_iji");
void wrap_imdct(CAacDecoderStaticChannelInfo *sci, CAacDecoderChannelInfo *ci, PCM_DEC out[], const SHORT len,
                const int ok, FIXP_DBL *work, const INT headroom, UINT el_flags, INT el_ch) {
  int64_t t = esp_timer_get_time();
  real_imdct(sci, ci, out, len, ok, work, headroom, el_flags, el_ch);
  g_phase_us[IMDCT] += esp_timer_get_time() - t;
  g_imdct_calls++;
}

extern "C" {
PCMDMX_ERROR __real_pcmDmx_ApplyFrame(HANDLE_PCM_DOWNMIX, DMX_PCM *, const int, UINT, INT *, INT,
                                      AUDIO_CHANNEL_TYPE[], UCHAR[], const FDK_channelMapDescr *const, INT *);
PCMDMX_ERROR __wrap_pcmDmx_ApplyFrame(HANDLE_PCM_DOWNMIX self, DMX_PCM *buf, const int size, UINT frame_size,
                                      INT *channels, INT interleaved, AUDIO_CHANNEL_TYPE types[], UCHAR idx[],
                                      const FDK_channelMapDescr *const map, INT *scale) {
  int64_t t = esp_timer_get_time();
  PCMDMX_ERROR e = __real_pcmDmx_ApplyFrame(self, buf, size, frame_size, channels, interleaved, types, idx, map, scale);
  g_phase_us[DMX] += esp_timer_get_time() - t;
  return e;
}
}

namespace esphome {
namespace fdk_bench {

static const char *const TAG = "fdk_bench";

struct Clip {
  const char *name;
  const uint8_t *data;
  size_t len;
};
static const Clip CLIPS[] = {
    {"stereo 44.1k 256k (aac_at)", clip_stereo_at, clip_stereo_at_len},
    {"5.1 48k 640k (aac_at)", clip_s51_at, clip_s51_at_len},
    {"5.1 48k 640k (ffmpeg)", clip_s51_ff, clip_s51_ff_len},
};
static constexpr int PASSES = 3;
static constexpr size_t OUT_FRAMES = 2048;

static void run_clip(const Clip &clip) {
  using namespace airplay_receiver;
  static int16_t out[OUT_FRAMES * 2];
  aac_fdk_t *dec = aac_fdk_open(2);
  if (dec == nullptr) {
    ESP_LOGE(TAG, "open failed");
    return;
  }
  int64_t total_us = 0, max_us = 0;
  uint32_t frames = 0, errors = 0, frame_size = 0, rate = 0, channels = 0;
  bool warmed_up = false;
  for (int pass = 0; pass < PASSES; pass++) {
    size_t pos = 0;
    while (pos + 7 <= clip.len) {
      const uint8_t *h = clip.data + pos;
      size_t len = ((h[3] & 0x03) << 11) | (h[4] << 3) | (h[5] >> 5);
      if (len < 7 || pos + len > clip.len) break;
      aac_fdk_frame_info_t info;
      int64_t t = esp_timer_get_time();
      int n = aac_fdk_decode(dec, h, len, out, OUT_FRAMES, &info);
      int64_t dt = esp_timer_get_time() - t;
      pos += len;
      // Let IDLE1 feed the task watchdog; the real decoder blocks on its queue between frames.
      vTaskDelay(1);
      if (n < 0) {
        errors++;
        continue;
      }
      if (!warmed_up) {
        // The first frame carries the decoder's configuration: not timed.
        warmed_up = true;
        frame_size = n, rate = info.sample_rate, channels = info.source_channels;
        g_imdct_calls = 0;
        for (auto &p : g_phase_us) p = 0;
        continue;
      }
      total_us += dt;
      if (dt > max_us) max_us = dt;
      frames++;
    }
  }
  aac_fdk_close(dec);
  if (frames == 0) {
    ESP_LOGE(TAG, "%s: no frames (errors=%u)", clip.name, (unsigned) errors);
    return;
  }
  const float avg = (float) total_us / frames / 1000.0f;
  const float budget = frame_size * 1000.0f / (rate ? rate : 1);
  float phase_ms[PHASES], phase_sum = 0;
  for (int i = 0; i < PHASES; i++) {
    phase_ms[i] = (float) g_phase_us[i] / frames / 1000.0f;
    phase_sum += phase_ms[i];
  }
  ESP_LOGI(TAG, "%s: ch=%u frames=%u err=%u avg=%.2f ms max=%.2f ms budget=%.1f ms load=%.0f%%", clip.name,
           (unsigned) channels, (unsigned) frames, (unsigned) errors, avg, max_us / 1000.0f, budget,
           100.0f * avg / budget);
  ESP_LOGI(TAG, "  read=%.2f (%.0f%%) tools=%.2f (%.0f%%) imdct=%.2f (%.0f%%, %.2f calls/frame) dmx=%.2f (%.0f%%) other=%.2f (%.0f%%)",
           phase_ms[READ], 100 * phase_ms[READ] / avg, phase_ms[TOOLS], 100 * phase_ms[TOOLS] / avg, phase_ms[IMDCT],
           100 * phase_ms[IMDCT] / avg, (float) g_imdct_calls / frames, phase_ms[DMX], 100 * phase_ms[DMX] / avg,
           avg - phase_sum, 100 * (avg - phase_sum) / avg);
}

static void bench_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(20000));
  for (int round = 1;; round++) {
    ESP_LOGI(TAG, "round %d on core %d, internal free=%u", round, xPortGetCoreID(),
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    for (const Clip &clip : CLIPS) run_clip(clip);
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
}

void FdkBench::setup() {
  // Same core, priority and stack as audio_decode (audio_decode_worker.cpp).
  xTaskCreatePinnedToCore(bench_task, "fdk_bench", 57344, nullptr, 6, nullptr, 1);
}

}  // namespace fdk_bench
}  // namespace esphome
