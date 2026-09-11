#pragma once
// airplay_receiver audio control surface.
//
// A small, self-contained C API that exposes the running audio engine
// (audio_output + audio_receiver) to an ESPHome media_player component so that
// Home Assistant can drive volume / play / pause / stop and query the current
// play state and track title. This is the layer that makes "audio is exposed
// to Home Assistant via media_player" real.
//
// LINKAGE: the seven airplay_audio_* functions are extern "C" at global scope.
// This is a deliberate ABI choice so an ESPHome media_player component (which
// lives in its own namespace) can call them by bare name
// (`airplay_audio_set_volume(...)`) with no using-directive or qualification,
// exactly as the media_player glue expects. The names are airplay_audio_*
// prefixed and unique, so they pollute no other namespace.
//
// THREAD SAFETY: every function may be called from the ESPHome loop or from any
// audio FreeRTOS task. The shared state guarded here (volume + track title) is
// protected by a portMUX_TYPE spinlock; the play/pause/stop/decode machinery
// the functions delegate to is already written to be invoked from multiple
// tasks by the rest of the engine.
//
// VOLUME SEMANTICS: AirPlay volume is carried by the transport as a Q15 gain
// (audio_output_set_volume_q15). This surface exposes a 0-100 scale and maps it
// to that Q15 gain (100 -> 32768 = unity, 0 -> mute). airplay_audio_get_volume()
// returns the last value set here (default 100). Note the RTSP client's own
// volume path (TRANSPORT_EVENT_VOLUME) continues to feed
// audio_output_set_volume_q15 directly and is a separate source of truth.
//
// TRACK TITLE: the RTSP METADATA event carries TransportMetadata (see
// transport/rtsp_events.h), which is a live event with no retained copy. The
// media_player / metadata consumer publishes the title here via
// airplay_audio_set_track_title(); airplay_audio_get_track_title() then returns
// it (or NULL when none has been set). This keeps the audio layer free of a
// transport dependency while still exposing a valid title.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the output/feed volume, 0-100 (clamped). Maps to the engine's Q15 gain.
 * @return true on success.
 */
bool airplay_audio_set_volume(uint8_t volume);

/**
 * Resume / start playback. Refuses (returns false) before the output backend
 * is initialized.
 * @return true if playback was started/resumed.
 */
bool airplay_audio_play(void);

/**
 * Pause playback while keeping the timing anchor (the output stage stays
 * running and is flushed, matching the PAUSED event handling).
 * @return true on success.
 */
bool airplay_audio_pause(void);

/**
 * Halt the stream and stop the output stage (matches the DISCONNECTED
 * handling).
 * @return true on success.
 */
bool airplay_audio_stop(void);

/**
 * True when playback is currently active (not paused).
 */
bool airplay_audio_is_playing(void);

/**
 * Current volume in 0-100 (the last value passed to airplay_audio_set_volume,
 * or 100 by default).
 */
int airplay_audio_get_volume(void);

/**
 * Current media track title (thread-safe snapshot), or NULL when none has been
 * published. The returned pointer stays valid until the next
 * airplay_audio_set_track_title() call.
 */
const char *airplay_audio_get_track_title(void);

/**
 * Publish the current track title (auxiliary control-surface hook the metadata
 * consumer / media_player calls). Copies it into an internal buffer.
 * @param title  Title string, or NULL to clear.
 */
void airplay_audio_set_track_title(const char *title);

#ifdef __cplusplus
}
#endif
