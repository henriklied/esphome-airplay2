// airplay_receiver audio control surface — implementation.
//
// Bridges the running audio engine (audio_output + audio_receiver) to the
// ESPHome media_player component via the exact airplay_audio_* signatures
// declared (extern "C", global scope) in audio_control.h. All functions are
// safe to call from the ESPHome loop and from any audio task: the volume +
// title state is guarded by a portMUX_TYPE spinlock, and the engine entry
// points they delegate to are already written to be invoked from multiple
// tasks.

#include "audio_control.h"

#include <cstdio>  // std::snprintf

#include "audio_output.h"
#include "audio_receiver.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

// The engine's public API lives in esphome::airplay_receiver; bring it into
// scope so the extern "C" functions below can call it unqualified.
using namespace esphome::airplay_receiver;

static const char *const TAG = "airplay_audio_control";

// Max track-title length (mirrors TRANSPORT_METADATA_STRING_MAX in the
// transport, kept local so this layer has no transport include dependency).
#define AIRPLAY_AUDIO_TITLE_MAX 64

// Shared control state (volume 0-100 + current track title). Written by the
// media_player / ESPHome loop and read back from the same tasks, so a spinlock
// keeps the multi-writer accesses coherent.
static uint8_t s_volume = 100;   // start at unity (100)
static char s_title[AIRPLAY_AUDIO_TITLE_MAX] = {};
static volatile bool s_title_valid = false;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Volume (0-100), mapped to the engine's Q15 gain
// ---------------------------------------------------------------------------
bool airplay_audio_set_volume(uint8_t volume) {
  if (volume > 100) {
    volume = 100;
  }
  // 100 -> 32768 (unity, bypasses the multiply in apply_volume); 0 -> mute.
  const int32_t volume_q15 = (int32_t) ((volume * 32768U + 50U) / 100U);
  audio_output_set_volume_q15(volume_q15);

  portENTER_CRITICAL(&s_lock);
  s_volume = volume;
  portEXIT_CRITICAL(&s_lock);
  return true;
}

int airplay_audio_get_volume(void) {
  portENTER_CRITICAL(&s_lock);
  const uint8_t v = s_volume;
  portEXIT_CRITICAL(&s_lock);
  return (int) v;
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------
bool airplay_audio_play(void) {
  // Refuse until the output backend is initialized, so a premature play() never
  // spawns a playback task that would write to a null I2S channel.
  if (!audio_output_is_ready()) {
    ESP_LOGW(TAG, "play() ignored: audio output not initialized");
    return false;
  }
  // Resume/start. audio_receiver_set_playing() is the engine's play/pause
  // latch; audio_output_start() is idempotent (no-op if the playback task is
  // already running).
  audio_receiver_set_playing(true);
  audio_output_start();
  return true;
}

bool airplay_audio_pause(void) {
  // Pause while preserving the timing anchor. The output stage stays running
  // (drains as silence) and is flushed — matches TRANSPORT_EVENT_PAUSED.
  audio_receiver_set_playing(false);
  audio_output_flush();
  return true;
}

bool airplay_audio_stop(void) {
  // Halt the stream and shut the output stage down — matches DISCONNECTED.
  audio_receiver_stop();
  audio_output_stop();
  return true;
}

bool airplay_audio_is_playing(void) { return audio_receiver_is_playing(); }

// ---------------------------------------------------------------------------
// Track title
// ---------------------------------------------------------------------------
const char *airplay_audio_get_track_title(void) {
  if (!s_title_valid) {
    return nullptr;
  }
  // Returns a stable pointer into the internal buffer; the caller should
  // consume it (report to Home Assistant) before the next set_track_title().
  return s_title;
}

void airplay_audio_set_track_title(const char *title) {
  portENTER_CRITICAL(&s_lock);
  if (title == nullptr) {
    s_title[0] = '\0';
    s_title_valid = false;
  } else {
    std::snprintf(s_title, sizeof(s_title), "%s", title);
    s_title_valid = true;
  }
  portEXIT_CRITICAL(&s_lock);
}
