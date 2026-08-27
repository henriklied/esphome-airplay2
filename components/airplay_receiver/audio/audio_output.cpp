// airplay_receiver I2S PCM5100 audio output backend.
//
// Port of upstream main/audio/audio_output.c (+ audio_output_common.c, whose
// "weak default" halves are folded in here as real implementations). The
// backend drives an external I2S stereo DAC (PCM5100) via the ESP-IDF standard
// I2S driver (/driver/i2s_std) and asserts a board amp-enable GPIO for the
// power amplifier. All heap routes through the airplay_* allocator; all logs go
// through esphome/core/log.h.
//
// Faithful to upstream's split:
//   * audio_output_start() spawns a playback task that PULLS decoded PCM from
//     the receiver ring via audio_receiver_read() (forward-declared below,
//     matching audio_receiver.h), applies resample / volume / channel mode and
//     writes the result to the I2S channel. audio_receiver_read is deliberately
//     NOT #included here to avoid pulling the receiver header's own include
//     chain (which reaches into crypto/crypto_module.h); only the declared sign
//     is needed and the definition comes from audio_receiver.cpp at link time.
//   * audio_output_write() writes straight to the I2S channel and is meant for
//     the "playback task stopped" case (e.g. a BT A2DP source), as upstream.
//
// Deviations from upstream, all documented:
//   * Channel mode is kept in RAM (upstream persisted to NVS via settings.h);
//     the NVS layer is a later task.
//   * DMA parameters (8 desc x 256 frame) match upstream's ~46 ms ring.
//   * The amp-enable GPIO is asserted in start() and de-asserted in stop().

#include "audio_output.h"

#include "audio_resample.h"
#include "../allocator.h"
#include "esphome/core/log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <cstdlib>

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "audio_output";

// Pull PCM from the receiver ring. Declared in audio_receiver.h (which we
// intentionally do not include here); the definition lives in audio_receiver.cpp
// and is resolved at link time.
size_t audio_receiver_read(int16_t *buffer, size_t samples);

// Playback frame granularity and resampling headroom. The playback task reads
// up to FRAME_SAMPLES + 1 frames from the receiver ring per iteration.
static constexpr size_t FRAME_SAMPLES = 352;

// DMA ring-buffer configuration. Total DMA latency (in samples) is
//   I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM
// which at the output rate gives the hardware pipeline delay in µs.
static constexpr int I2S_DMA_DESC_NUM = 8;
static constexpr int I2S_DMA_FRAME_NUM = 256;

// Max output frames after resampling one input frame (covers <=2x ratio).
static constexpr size_t MAX_RESAMPLE_FRAMES = (size_t) ((FRAME_SAMPLES + 2) * 2 + 16);

// Playback task core. On a single-core build the source tasks share core 0, so
// 0 keeps it colocated; on dual-core the source tasks sit on core 0 and the
// playback task runs on core 1 to avoid starving it.
#if CONFIG_FREERTOS_UNICORE
#define AIRPLAY_PLAYBACK_CORE 0
#else
#define AIRPLAY_PLAYBACK_CORE 1
#endif

// ---------------------------------------------------------------------------
// Backend state
// ---------------------------------------------------------------------------
static AudioOutputConfig g_config;
static bool g_config_set = false;
static uint32_t g_output_rate = 44100;

static i2s_chan_handle_t g_tx_handle = nullptr;
static volatile bool g_flush_requested = false;
static volatile bool g_playback_running = false;
static TaskHandle_t g_playback_task = nullptr;
static volatile int g_source_rate = 44100;
static volatile bool g_resample_reinit = false;
static volatile audio_channel_mode_t g_channel_mode = AUDIO_CHANNEL_STEREO;
static int32_t g_volume_q15 = 32768;  // unity

// Live output cursor. output_submitted_frames advances after a successful
// i2s_channel_write(); output_sent_frames is advanced by the TX DMA completion
// ISR. Their difference is the audio queued ahead of the next write — the real
// pipeline delay. auto_clear keeps the DMA clocking descriptors during a
// writer stall, so sent can overtake submitted; the excess (played as silence)
// is folded into output_lost_frames.
static uint64_t g_submitted_frames;
static uint64_t g_sent_frames;
static uint64_t g_lost_frames;
static uint32_t g_underruns;

static bool IRAM_ATTR on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
  (void) handle;
  (void) user_ctx;
  if (event && event->size > 0) {
    __atomic_add_fetch(&g_sent_frames, (uint64_t) (event->size / (2U * sizeof(int16_t))), __ATOMIC_RELAXED);
  }
  return false;
}

static void cursor_reset() {
  __atomic_store_n(&g_submitted_frames, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_sent_frames, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_lost_frames, 0, __ATOMIC_RELAXED);
}

// Frames queued in the DMA ring ahead of the next write. Reads the submitted/
// lost/sent counters atomically; the rebase below mutates lost + underruns but
// is only called from the timing engine (audio_output_get_pipeline_us), which
// is the sole reader of the queue depth.
static uint32_t queued_frames() {
  uint64_t submitted = __atomic_load_n(&g_submitted_frames, __ATOMIC_RELAXED);
  uint64_t lost = __atomic_load_n(&g_lost_frames, __ATOMIC_RELAXED);
  uint64_t sent = __atomic_load_n(&g_sent_frames, __ATOMIC_RELAXED);

  if (sent > submitted + lost) {
    // Ring ran dry: rebase so queued reads 0, remember how much time went out
    // as silence, and count an underrun.
    __atomic_store_n(&g_lost_frames, sent - submitted, __ATOMIC_RELAXED);
    g_underruns++;
    return 0;
  }

  uint64_t queued = submitted + lost - sent;
  constexpr uint64_t ring = (uint64_t) I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM;
  return queued > ring ? (uint32_t) ring : (uint32_t) queued;
}

// ---------------------------------------------------------------------------
// Amplifier enable
// ---------------------------------------------------------------------------
static void amp_set(bool on) {
  if (g_config.amp_enable_gpio < 0) {
    return;
  }
  int level = g_config.amp_enable_inverted ? (on ? 0 : 1) : (on ? 1 : 0);
  gpio_set_level((gpio_num_t) g_config.amp_enable_gpio, level);
}

// ---------------------------------------------------------------------------
// Volume / channel mode
// ---------------------------------------------------------------------------
// Ramp toward the target gain instead of applying volume changes instantly: an
// abrupt gain step mid-waveform is a discontinuity scaled by the signal's
// current amplitude (the classic volume "zipper" click). Step once per stereo
// frame so both channels always carry the same gain.
static void apply_volume(int16_t *buf, size_t samples) {
  if (g_volume_q15 == 32768) {
    return;
  }
  static int32_t cur_q15 = -1;
  int32_t target = g_volume_q15;
  if (cur_q15 < 0) {
    cur_q15 = target;  // first call: jump silently
  }
  for (size_t i = 0; i < samples; i++) {
    if ((i & 1) == 0 && cur_q15 != target) {
      int32_t diff = target - cur_q15;
      int32_t step = diff / 256;
      if (step == 0) {
        step = diff > 0 ? 1 : -1;
      }
      cur_q15 += step;
    }
    buf[i] = (int16_t) (((int32_t) buf[i] * cur_q15) >> 15);
  }
}

static void apply_channel_mode(int16_t *buf, size_t frames) {
  if (audio_output_channel_mode_in_dsp()) {
    return;
  }
  audio_channel_mode_t mode = g_channel_mode;
  if (mode == AUDIO_CHANNEL_STEREO) {
    return;
  }
  if (mode == AUDIO_CHANNEL_MONO) {
    for (size_t i = 0; i < frames; i++) {
      int16_t m = (int16_t) (((int32_t) buf[i * 2] + buf[i * 2 + 1]) / 2);
      buf[i * 2] = m;
      buf[i * 2 + 1] = m;
    }
    return;
  }
  size_t src = (mode == AUDIO_CHANNEL_RIGHT) ? 1 : 0;
  for (size_t i = 0; i < frames; i++) {
    int16_t s = buf[i * 2 + src];
    buf[i * 2] = s;
    buf[i * 2 + 1] = s;
  }
}

// ---------------------------------------------------------------------------
// Playback task — pulls decoded PCM from the receiver ring and drives I2S.
// ---------------------------------------------------------------------------
static void playback_task(void *arg) {
  (void) arg;
  // Real-time buffers (used during DMA writes) live in internal DRAM.
  int16_t *pcm = static_cast<int16_t *>(airplay_alloc((FRAME_SAMPLES + 1) * 2 * sizeof(int16_t), true));
  int16_t *silence = static_cast<int16_t *>(airplay_calloc(FRAME_SAMPLES * 2, sizeof(int16_t), true));
  int16_t *resample_buf = static_cast<int16_t *>(airplay_alloc(MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t), true));

  if (pcm == nullptr || silence == nullptr || resample_buf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate playback buffers");
    airplay_free(pcm);
    airplay_free(silence);
    airplay_free(resample_buf);
    g_playback_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  size_t written = 0;
  const size_t stride = 2 * sizeof(int16_t);  // bytes per stereo frame

  while (g_playback_running) {
    if (g_resample_reinit) {
      g_resample_reinit = false;
      audio_resample_init((uint32_t) g_source_rate, g_output_rate, 2);
    }
    if (g_flush_requested) {
      g_flush_requested = false;
      audio_resample_reset();
      cursor_reset();
      i2s_channel_disable(g_tx_handle);
      i2s_channel_enable(g_tx_handle);
    }

    size_t frames = audio_receiver_read(pcm, FRAME_SAMPLES + 1);
    if (frames > 0) {
      int16_t *play_buf = pcm;
      size_t play_frames = frames;
      if (audio_resample_is_active()) {
        play_frames = audio_resample_process(pcm, frames, resample_buf, MAX_RESAMPLE_FRAMES);
        play_buf = resample_buf;
      }
      apply_volume(play_buf, play_frames * 2);
      apply_channel_mode(play_buf, play_frames);
      if (i2s_channel_write(g_tx_handle, play_buf, play_frames * 2 * sizeof(int16_t), &written,
                            portMAX_DELAY) == ESP_OK) {
        __atomic_add_fetch(&g_submitted_frames, (uint64_t) (written / stride), __ATOMIC_RELAXED);
      }
      taskYIELD();
    } else {
      // Receiver underflow — output a frame of silence. Block on the DMA write
      // so the write itself paces the loop, instead of a short timeout plus
      // vTaskDelay(1) which produced jittery silence.
      if (i2s_channel_write(g_tx_handle, silence, (size_t) FRAME_SAMPLES * 2 * sizeof(int16_t), &written,
                            portMAX_DELAY) == ESP_OK) {
        __atomic_add_fetch(&g_submitted_frames, (uint64_t) (written / stride), __ATOMIC_RELAXED);
      }
    }
  }

  airplay_free(pcm);
  airplay_free(silence);
  airplay_free(resample_buf);
  g_playback_task = nullptr;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void audio_output_set_config(const AudioOutputConfig &config) {
  if (g_config_set) {
    ESP_LOGW(TAG, "audio_output_set_config called after init; ignoring");
    return;
  }
  g_config = config;
  g_output_rate = (config.sample_rate > 0) ? (uint32_t) config.sample_rate : 44100;
  g_config_set = true;
  ESP_LOGI(TAG, "Config: BCLK=%d LRCK=%d DOUT=%d AMP=%d rate=%d inverted=%d", config.i2s_bclk_gpio,
           config.i2s_lrclk_gpio, config.i2s_dout_gpio, config.amp_enable_gpio, config.sample_rate,
           config.amp_enable_inverted ? 1 : 0);
}

esp_err_t audio_output_init(void) {
  if (!g_config_set) {
    ESP_LOGE(TAG, "audio_output_init called before audio_output_set_config");
    return ESP_ERR_INVALID_STATE;
  }
  if (g_config.i2s_bclk_gpio < 0 || g_config.i2s_lrclk_gpio < 0 || g_config.i2s_dout_gpio < 0) {
    ESP_LOGE(TAG, "I2S BCLK/LRCK/DOUT GPIOs must be configured");
    return ESP_ERR_INVALID_ARG;
  }

  // Configure the amp-enable GPIO (de-asserted; start() asserts it).
  if (g_config.amp_enable_gpio >= 0) {
    gpio_reset_pin((gpio_num_t) g_config.amp_enable_gpio);
    gpio_set_direction((gpio_num_t) g_config.amp_enable_gpio, GPIO_MODE_OUTPUT);
    amp_set(false);
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
  chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;
  // Zero each DMA descriptor after it is sent. Without this a writer stall
  // longer than the ring makes the hardware replay stale ring contents in a
  // loop (loud stutter). With auto_clear an underrun degrades to silence.
  chan_cfg.auto_clear = true;

  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &g_tx_handle, nullptr), TAG, "channel create failed");

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(g_output_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t) g_config.i2s_bclk_gpio,
              .ws = (gpio_num_t) g_config.i2s_lrclk_gpio,
              .dout = (gpio_num_t) g_config.i2s_dout_gpio,
              .din = I2S_GPIO_UNUSED,
          },
  };

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_tx_handle, &std_cfg), TAG, "std mode init failed");

  // TX completion callback drives the live output cursor for the timing engine.
  // NOTE: field order matches i2s_event_callbacks_t (on_recv, on_recv_q_ovf,
  // on_sent, on_send_q_ovf).
  const i2s_event_callbacks_t callbacks = {
      .on_recv = nullptr,
      .on_recv_q_ovf = nullptr,
      .on_sent = on_sent,
      .on_send_q_ovf = nullptr,
  };
  ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(g_tx_handle, &callbacks, nullptr), TAG,
                      "event callback registration failed");
  cursor_reset();

  ESP_RETURN_ON_ERROR(i2s_channel_enable(g_tx_handle), TAG, "channel enable failed");
  ESP_LOGI(TAG, "I2S PCM5100 initialized: Rate=%u, DMA_Desc=%d, DMA_Frame=%d, Amp=%d",
           (unsigned int) g_output_rate, I2S_DMA_DESC_NUM, I2S_DMA_FRAME_NUM, g_config.amp_enable_gpio);

  audio_resample_init(44100, g_output_rate, 2);

  return ESP_OK;
}

void audio_output_start(void) {
  if (g_playback_task != nullptr) {
    return;  // already running
  }
  g_playback_running = true;
  // The DMA has been free-running since the last session, so the cursor carries
  // an arbitrary submitted/sent skew. Start the new session clean.
  cursor_reset();
  amp_set(true);
  xTaskCreatePinnedToCore(playback_task, "audio_play", 4096, nullptr, AIRPLAY_AUDIO_PLAYBACK_TASK_PRIORITY,
                          &g_playback_task, AIRPLAY_PLAYBACK_CORE);
}

void audio_output_stop(void) {
  if (g_playback_task == nullptr) {
    return;
  }
  g_playback_running = false;
  int timeout = 40;
  while (g_playback_task != nullptr && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  amp_set(false);
  if (g_playback_task != nullptr) {
    ESP_LOGW(TAG, "Playback task did not exit within timeout");
  } else {
    ESP_LOGI(TAG, "Playback task stopped");
  }
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  if (g_tx_handle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  // Direct write to I2S. Intended for the "playback task stopped" case (e.g. a
  // BT A2DP source); do not call concurrently with the running playback task.
  size_t written = 0;
  return i2s_channel_write(g_tx_handle, data, bytes, &written, wait);
}

void audio_output_set_sample_rate(uint32_t rate) {
  if (rate == 0 || g_tx_handle == nullptr) {
    return;
  }
  // Only safe when no writer task is actively using I2S (the caller must stop
  // the playback task first).
  ESP_LOGI(TAG, "Setting sample rate to %" PRIu32 " Hz", rate);
  i2s_channel_disable(g_tx_handle);
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  i2s_channel_reconfig_std_clock(g_tx_handle, &clk_cfg);
  g_output_rate = rate;
  g_resample_reinit = true;  // output rate changed: rebuild the resampler
  cursor_reset();
  i2s_channel_enable(g_tx_handle);
}

void audio_output_flush(void) {
  g_flush_requested = true;
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != g_source_rate) {
    g_source_rate = rate;
    g_resample_reinit = true;
  }
}

void audio_output_set_volume_q15(int32_t volume_q15) {
  g_volume_q15 = volume_q15;
}

uint32_t audio_output_get_hardware_latency_us(void) {
  if (g_output_rate == 0) {
    return 0;
  }
  // Model the midpoint of the DMA ring occupancy ahead of a new write: the
  // writer refills as soon as a descriptor completes, so steady-state
  // occupancy oscillates between (DESC_NUM-1) and DESC_NUM descriptors. Using
  // the full ring would overstate the delay by half a descriptor.
  return (uint32_t) (((uint64_t) (2 * I2S_DMA_DESC_NUM - 1) * I2S_DMA_FRAME_NUM * 1000000ULL / 2) /
                     g_output_rate);
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us) {
  uint32_t queued = queued_frames();
  if (now_us != nullptr) {
    *now_us = esp_timer_get_time();
  }
  if (pipeline_us != nullptr) {
    *pipeline_us = (uint32_t) (((uint64_t) queued * 1000000ULL) / g_output_rate);
  }
  return true;
}

uint32_t audio_output_get_underruns(void) {
  return __atomic_load_n(&g_underruns, __ATOMIC_RELAXED);
}

bool audio_output_channel_mode_locked(void) {
  // A single PCM5100 output does not fix the routing, so the mode is free.
  return false;
}

bool audio_output_channel_mode_in_dsp(void) {
  // The PCM5100 backend has no DSP flow making the channel selection, so the
  // software downmix handles it.
  return false;
}

audio_channel_mode_t audio_output_cycle_channel_mode(void) {
  if (audio_output_channel_mode_locked()) {
    return g_channel_mode;
  }
  audio_channel_mode_t next;
  switch (g_channel_mode) {
    case AUDIO_CHANNEL_STEREO:
      next = AUDIO_CHANNEL_LEFT;
      break;
    case AUDIO_CHANNEL_LEFT:
      next = AUDIO_CHANNEL_RIGHT;
      break;
    case AUDIO_CHANNEL_RIGHT:
      next = AUDIO_CHANNEL_MONO;
      break;
    default:
      next = AUDIO_CHANNEL_STEREO;
      break;
  }
  audio_output_set_channel_mode(next);
  return next;
}

void audio_output_set_channel_mode(audio_channel_mode_t mode) {
  if (audio_output_channel_mode_locked()) {
    return;
  }
  if (mode > AUDIO_CHANNEL_MONO) {
    mode = AUDIO_CHANNEL_STEREO;
  }
  g_channel_mode = mode;
  ESP_LOGI(TAG, "Channel mode: %s", mode == AUDIO_CHANNEL_LEFT     ? "LEFT only"
                                       : mode == AUDIO_CHANNEL_RIGHT ? "RIGHT only"
                                       : mode == AUDIO_CHANNEL_MONO  ? "MONO (L+R)"
                                                                      : "STEREO");
}

audio_channel_mode_t audio_output_get_channel_mode(void) {
  return g_channel_mode;
}

}  // namespace airplay_receiver
}  // namespace esphome
