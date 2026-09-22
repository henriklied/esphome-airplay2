// airplay_receiver S/PDIF audio output backend.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_output_spdif.c ("amedes"
// bit-banged S/PDIF). Only the DOUT GPIO carries the signal; BCLK/WS are
// internal-only. No external DAC or encoder chip is used — the 020A / DLT1120
// optical transmitter's DATA pin connects straight to spdif_dout_gpio.
//
// Same public audio_output_* API and same playback-task shape as the I2S
// backend (audio_output.cpp), so the receiver/timing layer links unchanged.
// Two deliberate differences from upstream's SPDIF file:
//   * the DSP stage (audio_dsp_process) runs before the encode, matching the
//     I2S backend so `dsp:` behaves identically on both outputs;
//   * volume comes from audio_output_set_volume_q15() with the same ramp as
//     the I2S backend, instead of a global airplay_get_volume_q15().
// The modelled-latency fallback is retained: the bit-bang path has no hardware
// completion cursor, so audio_output_get_pipeline_us() returns false and the
// timing engine falls back (see audio_output.h). Single-speaker playback is
// unaffected; multi-room sync is coarser than the I2S backend.

#ifdef AIRPLAY_OUTPUT_SPDIF

#include "audio_output.h"

#include "audio_dsp.h"
#include "audio_resample.h"
#include "../allocator.h"
#include "esphome/core/log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "audio_output_spdif";

// Pull PCM from the receiver ring (declared, not included — matches audio_output.cpp).
size_t audio_receiver_read(int16_t *buffer, size_t samples);
bool audio_receiver_last_read_was_silence(void);

static constexpr size_t FRAME_SAMPLES = 352;
// Worst-case resample headroom (<=2x ratio), matches the I2S backend.
static constexpr size_t MAX_RESAMPLE_FRAMES = (size_t) ((FRAME_SAMPLES + 2) * 2 + 16);

#ifndef AIRPLAY_I2S_PORT
#if defined(AIRPLAY_PLATFORM_ESP32S3)
#define AIRPLAY_I2S_PORT I2S_NUM_1
#else
#define AIRPLAY_I2S_PORT I2S_NUM_0
#endif
#endif

#if CONFIG_FREERTOS_UNICORE
#define AIRPLAY_PLAYBACK_CORE 0
#else
#define AIRPLAY_PLAYBACK_CORE 1
#endif

// ── SPDIF framing constants (verbatim from upstream) ───────────────────────
#define I2S_BITS      32
#define I2S_CHANNELS  2
#define BMC_BITS      64
#define BMC_FACTOR    (BMC_BITS / I2S_BITS)
#define SPDIF_BLOCK   192
#define SPDIF_BUF_DIV 2
#define DMA_BUF_COUNT  2
#define DMA_BUF_FRAMES (SPDIF_BLOCK * BMC_BITS / I2S_BITS / SPDIF_BUF_DIV)
#define SPDIF_BUF_BYTES (SPDIF_BLOCK * (BMC_BITS / 8) * I2S_CHANNELS / SPDIF_BUF_DIV)
#define SPDIF_BUF_WORDS (SPDIF_BUF_BYTES / sizeof(uint32_t))

#define SPDIF_DO_PIN g_config.spdif_dout_gpio

// ── Backend state ───────────────────────────────────────────────────────────
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
static int32_t g_volume_q15 = 32768;
static int32_t g_volume_ramp_q15 = -1;

// Amp idle power-down watchdog (audio_output.h amp_idle_timeout_ms). When the
// stream is active but produces no PCM frames for the whole timeout (pause or
// sustained underflow), the amp-enable line is de-asserted to mute/power down
// the amplifier; it is re-asserted on the next frame.
static uint32_t g_amp_idle_timeout_ms = 60000;  // 0 = disabled
static int64_t g_amp_idle_since_us = 0;         // ignored when g_amp_idle_muted
static bool g_amp_idle_muted = false;           // amp auto-muted by the watchdog

static uint32_t spdif_buf[SPDIF_BUF_WORDS];
static uint32_t *spdif_ptr;

// ── BMC preambles (verbatim from upstream) ──────────────────────────────────
#define BMC_B      0x33173333U /* block start (B) */
#define BMC_M      0x331d3333U /* left channel (M) */
#define BMC_W      0x331b3333U /* right channel (W) */
#define BMC_MW_DIF (BMC_M ^ BMC_W)

/* Byte offset within the first preamble word where M↔B differs */
#define SYNC_OFFSET 2
#define SYNC_FLIP   ((BMC_B ^ BMC_M) >> (SYNC_OFFSET * 8))

/* ── BMC lookup table ──────────────────────────────────────────────────
 * 8-bit PCM → 16-bit BMC, LSb first, ending with a "1" level.          */

// Upstream is C, where the >0x7fff entries below are plain int16_t bit
// patterns. C++ diagnoses those as narrowing in a braced initializer, so
// suppress the diagnostic here; the values and the int16_t type (whose sign
// extension spdif_write() relies on) are verbatim.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
// NOLINTBEGIN(bugprone-narrowing-conversions)
static const int16_t bmc_tab[256] = {
    0x3333, 0xb333, 0xd333, 0x5333, 0xcb33, 0x4b33, 0x2b33, 0xab33, 0xcd33,
    0x4d33, 0x2d33, 0xad33, 0x3533, 0xb533, 0xd533, 0x5533, 0xccb3, 0x4cb3,
    0x2cb3, 0xacb3, 0x34b3, 0xb4b3, 0xd4b3, 0x54b3, 0x32b3, 0xb2b3, 0xd2b3,
    0x52b3, 0xcab3, 0x4ab3, 0x2ab3, 0xaab3, 0xccd3, 0x4cd3, 0x2cd3, 0xacd3,
    0x34d3, 0xb4d3, 0xd4d3, 0x54d3, 0x32d3, 0xb2d3, 0xd2d3, 0x52d3, 0xcad3,
    0x4ad3, 0x2ad3, 0xaad3, 0x3353, 0xb353, 0xd353, 0x5353, 0xcb53, 0x4b53,
    0x2b53, 0xab53, 0xcd53, 0x4d53, 0x2d53, 0xad53, 0x3553, 0xb553, 0xd553,
    0x5553, 0xcccb, 0x4ccb, 0x2ccb, 0xaccb, 0x34cb, 0xb4cb, 0xd4cb, 0x54cb,
    0x32cb, 0xb2cb, 0xd2cb, 0x52cb, 0xcacb, 0x4acb, 0x2acb, 0xaacb, 0x334b,
    0xb34b, 0xd34b, 0x534b, 0xcb4b, 0x4b4b, 0x2b4b, 0xab4b, 0xcd4b, 0x4d4b,
    0x2d4b, 0xad4b, 0x354b, 0xb54b, 0xd54b, 0x554b, 0x332b, 0xb32b, 0xd32b,
    0x532b, 0xcb2b, 0x4b2b, 0x2b2b, 0xab2b, 0xcd2b, 0x4d2b, 0x2d2b, 0xad2b,
    0x352b, 0xb52b, 0xd52b, 0x552b, 0xccab, 0x4cab, 0x2cab, 0xacab, 0x34ab,
    0xb4ab, 0xd4ab, 0x54ab, 0x32ab, 0xb2ab, 0xd2ab, 0x52ab, 0xcaab, 0x4aab,
    0x2aab, 0xaaab, 0xcccd, 0x4ccd, 0x2ccd, 0xaccd, 0x34cd, 0xb4cd, 0xd4cd,
    0x54cd, 0x32cd, 0xb2cd, 0xd2cd, 0x52cd, 0xcacd, 0x4acd, 0x2acd, 0xaacd,
    0x334d, 0xb34d, 0xd34d, 0x534d, 0xcb4d, 0x4b4d, 0x2b4d, 0xab4d, 0xcd4d,
    0x4d4d, 0x2d4d, 0xad4d, 0x354d, 0xb54d, 0xd54d, 0x554d, 0x332d, 0xb32d,
    0xd32d, 0x532d, 0xcb2d, 0x4b2d, 0x2b2d, 0xab2d, 0xcd2d, 0x4d2d, 0x2d2d,
    0xad2d, 0x352d, 0xb52d, 0xd52d, 0x552d, 0xccad, 0x4cad, 0x2cad, 0xacad,
    0x34ad, 0xb4ad, 0xd4ad, 0x54ad, 0x32ad, 0xb2ad, 0xd2ad, 0x52ad, 0xcaad,
    0x4aad, 0x2aad, 0xaaad, 0x3335, 0xb335, 0xd335, 0x5335, 0xcb35, 0x4b35,
    0x2b35, 0xab35, 0xcd35, 0x4d35, 0x2d35, 0xad35, 0x3535, 0xb535, 0xd535,
    0x5535, 0xccb5, 0x4cb5, 0x2cb5, 0xacb5, 0x34b5, 0xb4b5, 0xd4b5, 0x54b5,
    0x32b5, 0xb2b5, 0xd2b5, 0x52b5, 0xcab5, 0x4ab5, 0x2ab5, 0xaab5, 0xccd5,
    0x4cd5, 0x2cd5, 0xacd5, 0x34d5, 0xb4d5, 0xd4d5, 0x54d5, 0x32d5, 0xb2d5,
    0xd2d5, 0x52d5, 0xcad5, 0x4ad5, 0x2ad5, 0xaad5, 0x3355, 0xb355, 0xd355,
    0x5355, 0xcb55, 0x4b55, 0x2b55, 0xab55, 0xcd55, 0x4d55, 0x2d55, 0xad55,
    0x3555, 0xb555, 0xd555, 0x5555,
};
// NOLINTEND(bugprone-narrowing-conversions)
#pragma GCC diagnostic pop

/* ── SPDIF buffer init ─────────────────────────────────────────────────
 * Pre-fill even indices with alternating M / W preamble words.
 * Odd indices (audio data) will be overwritten during conversion.       */

static void spdif_buf_init(void) {
  uint32_t bmc_mw = BMC_W;
  for (int i = 0; i < (int) SPDIF_BUF_WORDS; i += 2) {
    spdif_buf[i] = (bmc_mw ^= BMC_MW_DIF);
  }
}

// ── Volume / channel mode (from the I2S backend, using local statics) ──────
// Ramp toward the target gain instead of applying volume changes instantly: an
// abrupt gain step mid-waveform is a discontinuity scaled by the signal's
// current amplitude (the classic volume "zipper" click). Step once per stereo
// frame so both channels always carry the same gain.
static void apply_volume(int16_t *buf, size_t samples) {
  int32_t target = g_volume_q15;
  if (g_volume_ramp_q15 < 0) {
    g_volume_ramp_q15 = target;  // first frames of a session: adopt silently
  }
  // Only skip the scaling loop once the ramp has actually arrived at unity.
  // Returning early on target == unity alone would leave the ramp stale and
  // turn every move to or from full volume into an unramped step -- a click.
  if (g_volume_ramp_q15 == target && target == 32768) {
    return;
  }
  for (size_t i = 0; i < samples; i++) {
    if ((i & 1) == 0 && g_volume_ramp_q15 != target) {
      int32_t diff = target - g_volume_ramp_q15;
      int32_t step = diff / 256;
      if (step == 0) {
        step = diff > 0 ? 1 : -1;
      }
      g_volume_ramp_q15 += step;
    }
    buf[i] = (int16_t) (((int32_t) buf[i] * g_volume_ramp_q15) >> 15);
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

// ── SPDIF write ─────────────────────────────────────────────────────────────
static void spdif_write(const void *src, size_t size) {
  const uint8_t *p = (const uint8_t *) src;
  while (p < (const uint8_t *) src + size) {
    *(spdif_ptr + 1) = (uint32_t) (((bmc_tab[*p] << 16) ^ bmc_tab[*(p + 1)]) << 1) >> 1;
    p += 2;
    spdif_ptr += 2;
    if (spdif_ptr >= &spdif_buf[SPDIF_BUF_WORDS]) {
      size_t written;
      ((uint8_t *) spdif_buf)[SYNC_OFFSET] ^= SYNC_FLIP;
      i2s_channel_write(g_tx_handle, spdif_buf, sizeof(spdif_buf), &written, portMAX_DELAY);
      spdif_ptr = spdif_buf;
    }
  }
}

static void amp_set(bool on) {
  if (g_config.amp_enable_gpio < 0) {
    return;
  }
  int level = g_config.amp_enable_inverted ? (on ? 0 : 1) : (on ? 1 : 0);
  gpio_set_level((gpio_num_t) g_config.amp_enable_gpio, level);
}

// ── Playback task ───────────────────────────────────────────────────────────
static void playback_task(void *arg) {
  (void) arg;
  int16_t *pcm = (int16_t *) airplay_alloc((FRAME_SAMPLES + 1) * 2 * sizeof(int16_t), true);
  int16_t *silence = (int16_t *) airplay_calloc(FRAME_SAMPLES * 2, sizeof(int16_t), true);
  int16_t *resample_buf = (int16_t *) airplay_alloc(MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t), true);
  if (pcm == nullptr || silence == nullptr || resample_buf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate playback buffers");
    airplay_free(pcm);
    airplay_free(silence);
    airplay_free(resample_buf);
    g_playback_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  while (g_playback_running) {
    if (g_resample_reinit) {
      g_resample_reinit = false;
      audio_resample_init((uint32_t) g_source_rate, g_output_rate, 2);
    }
    if (g_flush_requested) {
      g_flush_requested = false;
      audio_resample_reset();
      audio_dsp_reset();
      i2s_channel_disable(g_tx_handle);
      spdif_buf_init();
      spdif_ptr = spdif_buf;
      i2s_channel_enable(g_tx_handle);
    }
    size_t samples = audio_receiver_read(pcm, FRAME_SAMPLES + 1);
    // A frame count is not evidence of audio.  The scheduler answers "I cannot
    // play" by zero-filling the buffer and returning the FULL count, so a
    // stream wedged with no anchor (no PTP lock -> no clock map) looks exactly
    // like a healthy one here.  Keying the idle watchdog off `samples > 0`
    // therefore held the amp powered indefinitely through a fault whose whole
    // symptom is that nothing comes out of it.
    const bool carrying_audio =
        samples > 0 && !audio_receiver_last_read_was_silence();

    if (carrying_audio) {
      // Data is flowing again: cancel any idle power-down and re-assert the
      // amp if the watchdog had muted it (e.g. resume after a long pause).
      if (g_amp_idle_muted) {
        amp_set(true);
        g_amp_idle_muted = false;
      }
      g_amp_idle_since_us = 0;
    } else if (g_amp_idle_timeout_ms > 0) {
      // Silent for the full timeout -- a pause, a sustained stall, or a wedge.
      if (g_amp_idle_since_us == 0) {
        g_amp_idle_since_us = esp_timer_get_time();
      } else if (esp_timer_get_time() - g_amp_idle_since_us >=
                 (int64_t) g_amp_idle_timeout_ms * 1000LL) {
        if (!g_amp_idle_muted) {
          amp_set(false);
          g_amp_idle_muted = true;
        }
      }
    } else {
      g_amp_idle_since_us = 0;
    }

    if (samples > 0) {
      int16_t *play_buf = pcm;
      size_t play_samples = samples;
      if (audio_resample_is_active()) {
        play_samples = audio_resample_process(pcm, samples, resample_buf, MAX_RESAMPLE_FRAMES);
        play_buf = resample_buf;
      }
      apply_volume(play_buf, play_samples * 2);
      apply_channel_mode(play_buf, play_samples);
      audio_dsp_process(play_buf, play_samples);
      spdif_write(play_buf, play_samples * 2 * sizeof(int16_t));
      taskYIELD();
    } else {
      spdif_write(silence, (size_t) FRAME_SAMPLES * 2 * sizeof(int16_t));
      vTaskDelay(1);
    }
  }

  airplay_free(pcm);
  airplay_free(silence);
  airplay_free(resample_buf);
  g_playback_task = nullptr;
  vTaskDelete(nullptr);
}

// ── Public API ──────────────────────────────────────────────────────────────
void audio_output_set_config(const AudioOutputConfig &config) {
  if (g_config_set) {
    ESP_LOGW(TAG, "audio_output_set_config called after init; ignoring");
    return;
  }
  g_config = config;
  g_output_rate = (config.sample_rate > 0) ? (uint32_t) config.sample_rate : 44100;
  g_amp_idle_timeout_ms = config.amp_idle_timeout_ms;
  g_config_set = true;
  ESP_LOGI(TAG, "Config: SPDIF DOUT=%d AMP=%d rate=%d inverted=%d idle_timeout=%lu ms", config.spdif_dout_gpio,
           config.amp_enable_gpio, config.sample_rate, config.amp_enable_inverted ? 1 : 0,
           (unsigned long) config.amp_idle_timeout_ms);
}

esp_err_t audio_output_init(void) {
  if (!g_config_set) {
    ESP_LOGE(TAG, "audio_output_init called before audio_output_set_config");
    return ESP_ERR_INVALID_STATE;
  }
  if (g_config.spdif_dout_gpio < 0) {
    ESP_LOGE(TAG, "spdif_dout_gpio must be configured for the SPDIF backend");
    return ESP_ERR_INVALID_ARG;
  }

  if (g_config.amp_enable_gpio >= 0) {
    gpio_reset_pin((gpio_num_t) g_config.amp_enable_gpio);
    gpio_set_direction((gpio_num_t) g_config.amp_enable_gpio, GPIO_MODE_OUTPUT);
    amp_set(false);
  }

  spdif_buf_init();
  spdif_ptr = spdif_buf;

  const i2s_port_t i2s_port = (i2s_port_t) AIRPLAY_I2S_PORT;
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(i2s_port, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = DMA_BUF_COUNT;
  chan_cfg.dma_frame_num = DMA_BUF_FRAMES;
  // Zero each DMA descriptor after it is sent. Without this a writer stall
  // longer than the ring makes the hardware replay stale ring contents in a
  // loop (loud stutter). With auto_clear an underrun degrades to silence.
  chan_cfg.auto_clear = true;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &g_tx_handle, nullptr), TAG, "channel create failed");

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(g_output_rate * BMC_FACTOR);
#if SOC_I2S_SUPPORTS_APLL
  clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif
  clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  i2s_std_config_t std_cfg = {
      .clk_cfg = clk_cfg,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = I2S_GPIO_UNUSED,
              .ws = I2S_GPIO_UNUSED,
              .dout = (gpio_num_t) SPDIF_DO_PIN,
              .din = I2S_GPIO_UNUSED,
          },
  };
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_tx_handle, &std_cfg), TAG, "std mode init failed");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(g_tx_handle), TAG, "channel enable failed");

  // Pre-fill DMA with SPDIF-encoded silence so the receiver can lock.
  {
    int16_t silence_pcm[SPDIF_BLOCK * 2];
    memset(silence_pcm, 0, sizeof(silence_pcm));
    for (int i = 0; i < DMA_BUF_COUNT; i++) {
      spdif_write(silence_pcm, (size_t) (SPDIF_BLOCK / SPDIF_BUF_DIV) * 2 * sizeof(int16_t));
    }
  }

  audio_resample_init(44100, g_output_rate, 2);
  audio_dsp_set_sample_rate(g_output_rate);
  ESP_LOGI(TAG, "SPDIF output ready: GPIO=%d rate=%u x%d dma=%dx%d", SPDIF_DO_PIN, (unsigned) g_output_rate,
           BMC_FACTOR, DMA_BUF_FRAMES, DMA_BUF_COUNT);
  return ESP_OK;
}

bool audio_output_is_ready(void) { return g_config_set && g_tx_handle != nullptr; }

void audio_output_start(void) {
  if (g_playback_task != nullptr) {
    return;
  }
  g_playback_running = true;
  g_volume_ramp_q15 = -1;
  g_amp_idle_since_us = 0;
  g_amp_idle_muted = false;
  audio_dsp_reset();
  amp_set(true);
  xTaskCreatePinnedToCore(playback_task, "spdif_play", 4096, nullptr, AIRPLAY_AUDIO_PLAYBACK_TASK_PRIORITY,
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
  g_amp_idle_since_us = 0;
  g_amp_idle_muted = false;
  amp_set(false);
}

void audio_output_flush(void) { g_flush_requested = true; }

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  (void) wait;
  if (g_tx_handle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  spdif_write(data, bytes);
  return ESP_OK;
}

void audio_output_set_sample_rate(uint32_t rate) {
  if (rate == 0 || g_tx_handle == nullptr) {
    return;
  }
  // Only safe when no writer task is actively using I2S (the caller must stop
  // the playback task first).
  i2s_channel_disable(g_tx_handle);
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate * BMC_FACTOR);
  i2s_channel_reconfig_std_clock(g_tx_handle, &clk_cfg);
  g_output_rate = rate;
  g_resample_reinit = true;
  audio_dsp_set_sample_rate(rate);
  i2s_channel_enable(g_tx_handle);
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != g_source_rate) {
    g_source_rate = rate;
    g_resample_reinit = true;
  }
}

void audio_output_set_volume_q15(int32_t volume_q15) { g_volume_q15 = volume_q15; }

uint32_t audio_output_get_hardware_latency_us(void) {
  const uint32_t audio_samples = DMA_BUF_COUNT * (SPDIF_BLOCK / SPDIF_BUF_DIV);
  return (uint32_t) ((uint64_t) audio_samples * 1000000ULL / g_output_rate);
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us) {
  // No hardware completion cursor on the bit-bang path: the timing engine falls
  // back to the modelled latency (audio_output_get_next_playout_time_ns).
  (void) now_us;
  (void) pipeline_us;
  return false;
}

int64_t audio_output_get_next_playout_time_ns(int64_t now_us) {
  return (now_us + (int64_t) audio_output_get_hardware_latency_us() + 5000) * 1000LL;
}

uint32_t audio_output_get_underruns(void) { return 0; }

bool audio_output_channel_mode_locked(void) { return false; }
bool audio_output_channel_mode_in_dsp(void) { return false; }

audio_channel_mode_t audio_output_cycle_channel_mode(void) {
  audio_channel_mode_t next;
  switch (g_channel_mode) {
    case AUDIO_CHANNEL_STEREO: next = AUDIO_CHANNEL_LEFT; break;
    case AUDIO_CHANNEL_LEFT: next = AUDIO_CHANNEL_RIGHT; break;
    case AUDIO_CHANNEL_RIGHT: next = AUDIO_CHANNEL_MONO; break;
    default: next = AUDIO_CHANNEL_STEREO; break;
  }
  audio_output_set_channel_mode(next);
  return next;
}

void audio_output_set_channel_mode(audio_channel_mode_t mode) {
  if (mode > AUDIO_CHANNEL_MONO) {
    mode = AUDIO_CHANNEL_STEREO;
  }
  g_channel_mode = mode;
}

audio_channel_mode_t audio_output_get_channel_mode(void) { return g_channel_mode; }

}  // namespace airplay_receiver
}  // namespace esphome

#endif  // AIRPLAY_OUTPUT_SPDIF
