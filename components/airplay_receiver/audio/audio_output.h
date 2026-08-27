#pragma once
// airplay_receiver audio output backend.
//
// Port of upstream main/audio/audio_output.c + audio_output_common.c for an
// I2S external DAC (PCM5100) driven through the ESP-IDF standard I2S driver,
// plus a board amp-enable GPIO. All heap routes through ../allocator.h
// (airplay_alloc / airplay_calloc / airplay_free); all logging uses the
// ESPHome log macros and a per-file TAG.
//
// The output stage is a self-contained leaf: the receiver/timing layer calls
// audio_output_* (see below), and the internal playback task drains a feed
// FIFO populated by audio_output_write(). The upstream audio_output_* public
// API is preserved verbatim so the timing engine can query hardware latency,
// the live pipeline depth and underruns, and so the receiver can feed audio.

#include <cstdbool>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace esphome {
namespace airplay_receiver {

/**
 * Priority of the playback task. It MUST outrank every audio source task
 * (realtime UDP receiver = 8, control receiver = 7, buffered TCP reader = 5)
 * because the source tasks are pinned to the same core. A source task that
 * outranks playback starves it during a receive burst; the DMA ring (~46 ms)
 * then runs dry and auto_clear emits silence, so wall-clock advances while no
 * audio is consumed and the playout position slips permanently late.
 */
#define AIRPLAY_AUDIO_PLAYBACK_TASK_PRIORITY 9

/**
 * Output channel mode. LEFT/RIGHT route the chosen source channel to both
 * speakers; MONO plays the (L+R)/2 downmix on both speakers; STEREO (default)
 * plays the normal left/right mix.
 */
typedef enum {
  AUDIO_CHANNEL_STEREO = 0,
  AUDIO_CHANNEL_LEFT,
  AUDIO_CHANNEL_RIGHT,
  AUDIO_CHANNEL_MONO,
} audio_channel_mode_t;

/**
 * Board wiring + output configuration for the I2S PCM5100 backend.
 *
 * The component builds this from the YAML config and hands it to the backend
 * via audio_output_set_config() before audio_output_init().
 */
struct AudioOutputConfig {
  /// BCLK (bit clock) GPIO for the I2S bus.
  int i2s_bclk_gpio = -1;
  /// LRCK (word/frame clock, "WS") GPIO.
  int i2s_lrclk_gpio = -1;
  /// DOUT (serial data out to the DAC) GPIO.
  int i2s_dout_gpio = -1;
  /// Board amp-enable GPIO (drives the PA/amp power line).
  int amp_enable_gpio = -1;
  /// Output sample rate in Hz (e.g. 44100, 48000).
  int sample_rate = 44100;
  /// True if the amp-enable line is active-LOW (default: active-HIGH).
  bool amp_enable_inverted = false;
};

/**
 * Supply the backend configuration (pins + sample rate) before init().
 * Only valid before audio_output_init(); ignored afterwards.
 */
void audio_output_set_config(const AudioOutputConfig &config);

/**
 * Initialize the I2S PCM5100 output backend.
 */
esp_err_t audio_output_init(void);

/**
 * Start the audio playback task (drains the feed FIFO to I2S).
 */
void audio_output_start(void);

/**
 * Flush output buffers (clears stale audio on pause/seek).
 */
void audio_output_flush(void);

/**
 * Stop the playback task and de-assert the amp-enable GPIO.
 */
void audio_output_stop(void);

/**
 * Write raw PCM to the output.
 *
 * When the playback task is running the bytes are queued into the feed FIFO
 * drained by that task (which applies resampling / volume / channel mode
 * before the I2S write). When it is stopped the bytes are written straight to
 * the I2S channel (e.g. a BT A2DP source).
 *
 * @param data   PCM data buffer (interleaved stereo, 16-bit)
 * @param bytes  Number of bytes to write
 * @param wait   Maximum ticks to wait for feed/space
 * @return ESP_OK on success
 */
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);

/**
 * Change the I2S output sample rate (e.g. when BT negotiates 48 kHz).
 * @param rate  Sample rate in Hz.
 */
void audio_output_set_sample_rate(uint32_t rate);

/**
 * Notify the output of the source sample rate (from AirPlay ANNOUNCE).
 * The resampler is re-initialized if the rate changes.
 */
void audio_output_set_source_rate(int rate);

/**
 * Set the feed volume as a Q15 scale factor (0 = mute, 32768 = unity).
 * Additive extension over upstream (which reads a global volume); the receiver
 * calls this so the output stage can ramp volume like upstream's apply_volume.
 * A value of 32768 skips the per-sample multiply.
 */
void audio_output_set_volume_q15(int32_t volume_q15);

/**
 * Return the modelled I2S DMA pipeline latency in microseconds.
 *   (2 * dma_desc_num - 1) * dma_frame_num * 1e6 / (2 * sample_rate)
 */
uint32_t audio_output_get_hardware_latency_us(void);

/**
 * Sample the live output pipeline delay: how long from now until the first
 * sample of the NEXT backend write is heard. Uses the queue depth reported by
 * the hardware completion cursor.
 *
 * @param now_us       out: esp_timer_get_time() sampled with the queue depth.
 * @param pipeline_us  out: queue depth in microseconds at the output rate.
 * @return true (the backend always has a completion cursor).
 */
bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us);

/**
 * Number of output-underrun episodes since boot.
 */
uint32_t audio_output_get_underruns(void);

/**
 * Cycle the output channel mode: STEREO -> LEFT -> RIGHT -> MONO -> STEREO.
 * @return the new mode after cycling.
 */
audio_channel_mode_t audio_output_cycle_channel_mode(void);

/**
 * Set the output channel mode directly.
 */
void audio_output_set_channel_mode(audio_channel_mode_t mode);

/**
 * Get the current output channel mode.
 */
audio_channel_mode_t audio_output_get_channel_mode(void);

/**
 * True when the DAC configuration already fixes the per-output routing. For a
 * single PCM5100 backend this is always false (routing is free).
 */
bool audio_output_channel_mode_locked(void);

/**
 * True when a DSP flow makes the channel selection instead of the software
 * downmix. The PCM5100 backend has no DSP, so this is always false.
 */
bool audio_output_channel_mode_in_dsp(void);

}  // namespace airplay_receiver
}  // namespace esphome
