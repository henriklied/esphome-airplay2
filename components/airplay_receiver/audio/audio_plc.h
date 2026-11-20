#pragma once

// Packet-loss concealment for timeline holes.
//
// Realtime AirPlay from iOS loses 2-40 packet bursts (16-320 ms) that are never
// resent.  A hole is filled with audio, not silence: the audio before it is
// extended forward by repeating its last pitch-matched period, the audio after
// it (already buffered, playout runs ~2 s behind arrival) is extended backward
// the same way, and the two are crossfaded across the hole.  Without audio
// after the hole the forward extension holds briefly and fades out.
//
// Frame counts below assume 44.1 kHz, the AirPlay rate.  Pure C++ with no
// platform dependencies so it builds on the host for tests.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/// Interleaving of every internal buffer; mono is stored duplicated.
static constexpr uint32_t PLC_STRIDE = 2;
static constexpr uint32_t PLC_HISTORY_FRAMES = 4096;
static constexpr uint32_t PLC_FUTURE_FRAMES = 2048;
/// Longest hole synthesized (500 ms); the rest of a longer hole is silence.
static constexpr uint32_t PLC_MAX_FILL_FRAMES = 22050;
/// Repetition period search range, 10-30 ms.
static constexpr uint32_t PLC_MIN_PERIOD = 441;
static constexpr uint32_t PLC_MAX_PERIOD = 1323;
/// Correlation window of the period search, 15 ms.
static constexpr uint32_t PLC_MATCH_FRAMES = 661;
/// Coarse search runs on the signal decimated by this factor.
static constexpr uint32_t PLC_DECIMATION = 8;
/// Window of the full-rate refinement around the coarse lag, 7.5 ms.
static constexpr uint32_t PLC_FINE_FRAMES = 330;
/// Frames of history the search and the forward loop can use.
static constexpr uint32_t PLC_PAST_FRAMES = 2U * PLC_MAX_PERIOD + PLC_MATCH_FRAMES;
/// Blend at loop seams and into resumed audio, 2 ms.
static constexpr uint32_t PLC_SEAM_FRAMES = 88;
/// Fade length of each side in a two-sided fill, 120 ms.
static constexpr uint32_t PLC_CROSSFADE_FRAMES = 5292;
/// Forward-only fill: full level for 20 ms, then fade out over 100 ms.
static constexpr uint32_t PLC_HOLD_FRAMES = 882;
static constexpr uint32_t PLC_FADE_FRAMES = 4410;

/* Plain data: audio_timeline_t is zeroed with memset.  The fill is
 * synthesized frame by frame as it is played, so a long hole costs the same
 * per render as a short one. */
struct audio_plc_t {
  int16_t *history;   // ring of the last PLC_HISTORY_FRAMES emitted frames
  int16_t *past;      // history unrolled oldest-first at the hole start
  int16_t *future;    // audio after the hole, filled by the timeline
  float *analysis;    // decimated stereo for the coarse period search
  uint32_t history_head;
  uint32_t history_count;
  uint32_t past_frames;
  uint32_t future_frames;
  uint32_t forward_period;   // 0: hold the last sample instead of looping
  uint32_t backward_period;  // 0: hold the first future sample
  uint32_t fill_frames;      // planned hole, capped at PLC_MAX_FILL_FRAMES
  uint32_t fill_pos;
  uint32_t blend_pos;  // PLC_SEAM_FRAMES when no blend is running
  bool blend_from_tail;
  bool active;
  bool end_known;
  int16_t last[PLC_STRIDE];
  int16_t blend_hold[PLC_STRIDE];  // blend source once the fill is exhausted
  int16_t forward_hold[PLC_STRIDE];  // last frame before the hole
  uint32_t two_sided_events;
  uint32_t forward_events;
};

/// Allocates the buffers.  On false the PLC stays usable and conceals with
/// silence.
bool audio_plc_init(audio_plc_t *plc);
void audio_plc_deinit(audio_plc_t *plc);
/// Forgets history and any hole in progress (stream discontinuity).
void audio_plc_reset(audio_plc_t *plc);

/// Plans a hole of `hole_frames`.  `future_frames` frames of the audio that
/// follows it are in plc->future; 0 means the end is unknown.
void audio_plc_begin(audio_plc_t *plc, uint32_t hole_frames,
                     uint32_t future_frames);
/// Emits the next `frames` of the planned hole.
void audio_plc_conceal(audio_plc_t *plc, int16_t *out, size_t frames,
                       uint8_t channels);
/// Passes real PCM through, blending the first frames after a hole.
void audio_plc_recover(audio_plc_t *plc, int16_t *pcm, size_t frames,
                       uint8_t channels);

/// Repetition period of `frames` stride-2 frames, matched at the end
/// (`at_end`) or the start.  `scratch` holds frames / PLC_DECIMATION *
/// PLC_STRIDE floats.
/// Returns 0 when fewer than PLC_MIN_PERIOD + PLC_MATCH_FRAMES are available.
uint32_t audio_plc_find_period(const int16_t *pcm, uint32_t frames, bool at_end,
                               float *scratch);

}  // namespace airplay_receiver
}  // namespace esphome
