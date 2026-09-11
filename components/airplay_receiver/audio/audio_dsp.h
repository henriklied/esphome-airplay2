#pragma once
// airplay_receiver output DSP: a cascade of RBJ biquads on the playout path.
//
// This exists because direct AirPlay from an iPhone or Mac never touches Music
// Assistant, so MA's per-player DSP (parametric EQ, high-pass, limiter) does
// not apply. Anything the speaker needs to sound right -- a protective
// high-pass, a low-shelf lift -- has to run on the board itself.
//
// The stage sits last in playback_task, after volume and channel mode, so the
// filters always see exactly the samples that reach the DAC. Running after the
// volume attenuation is deliberate: a shelf with positive gain adds headroom
// pressure, and attenuating first keeps the boost from clipping at high volume.
//
// Threading contract -- this is the part that matters:
//   * audio_dsp_process() and audio_dsp_reset() run ON the playback task. They
//     never allocate, never take a lock, and never call libm.
//   * Every audio_dsp_set_*() recomputes coefficients (sin/cos/pow) and MUST be
//     called from a non-realtime context -- component setup() or the ESPHome
//     main loop. They publish by filling the inactive coefficient bank and then
//     flipping a single index, so the audio task always reads one coherent
//     bank without blocking.
// Filter *state* deliberately lives outside the banks and survives a
// coefficient swap; zeroing it on every edit would click on each knob turn.

#include <cstdbool>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/// Maximum biquad sections in the cascade. Eight is well past what a two-way
/// speaker correction needs and still only ~1% of one core at 44.1 kHz stereo.
#define AIRPLAY_DSP_MAX_FILTERS 8

/**
 * Biquad response shape. Values match the YAML `type:` enum in __init__.py --
 * keep the two in step.
 */
typedef enum {
  AIRPLAY_DSP_LOW_SHELF = 0,
  AIRPLAY_DSP_HIGH_SHELF,
  AIRPLAY_DSP_HIGH_PASS,
  AIRPLAY_DSP_LOW_PASS,
  AIRPLAY_DSP_PEAKING,
  AIRPLAY_DSP_NOTCH,
} airplay_dsp_filter_type_t;

/**
 * One filter as configured, in human units. Held so a single parameter can be
 * changed at runtime without the caller having to restate the rest.
 */
struct AirPlayDspFilter {
  airplay_dsp_filter_type_t type = AIRPLAY_DSP_PEAKING;
  float frequency_hz = 1000.0f;
  float q = 0.7071f;
  /// Shelf/peaking gain in dB. Ignored by high-pass, low-pass and notch.
  float gain_db = 0.0f;
};

/**
 * Set the output sample rate the coefficients are designed for. Recomputes the
 * whole cascade. Call before the first audio_dsp_process() and again whenever
 * the output rate changes.
 */
void audio_dsp_set_sample_rate(uint32_t sample_rate);

/**
 * Replace the filter cascade. `count` is clamped to AIRPLAY_DSP_MAX_FILTERS.
 * A count of 0 leaves only the preamp gain in the path.
 */
void audio_dsp_set_filters(const AirPlayDspFilter *filters, size_t count);

/**
 * Update one section in place, keeping the others. Out-of-range indices below
 * AIRPLAY_DSP_MAX_FILTERS extend the cascade; anything beyond is ignored.
 */
void audio_dsp_set_filter(size_t index, const AirPlayDspFilter &filter);

/**
 * Read back a section as configured (for a runtime edit that changes only one
 * parameter). Returns false if `index` is beyond the active cascade.
 */
bool audio_dsp_get_filter(size_t index, AirPlayDspFilter *out);

/// Number of sections currently in the cascade.
size_t audio_dsp_get_filter_count(void);

/**
 * Broadband gain applied ahead of the cascade, in dB. Negative values buy back
 * the headroom a positive shelf or peaking gain spends; without it a boost
 * clips into the amplifier at high volume.
 */
void audio_dsp_set_preamp_db(float preamp_db);

/// Current preamp in dB.
float audio_dsp_get_preamp_db(void);

/**
 * Bypass the whole stage. Bypassed is bit-exact passthrough (not even the
 * preamp), so it is a true A/B against the unfiltered signal.
 */
void audio_dsp_set_enabled(bool enabled);
bool audio_dsp_is_enabled(void);

/**
 * True when the stage would alter the signal: enabled, and either a preamp
 * other than 0 dB or at least one section. Lets the caller skip the whole
 * conversion when nothing is configured.
 */
bool audio_dsp_is_active(void);

/**
 * Clear all filter state. Call on flush/seek -- stale biquad state played into
 * a new stream position is an audible thump. Safe on the playback task.
 */
void audio_dsp_reset(void);

/**
 * Filter a block of interleaved stereo 16-bit PCM in place.
 *
 * Runs on the playback task. Samples are widened to float, run through the
 * cascade per channel, then rounded and clamped back to int16.
 *
 * @param buf     Interleaved stereo int16 samples.
 * @param frames  Stereo frames (buf holds 2 * frames samples).
 */
void audio_dsp_process(int16_t *buf, size_t frames);

/**
 * Peak sample magnitude seen at the cascade output since the last call, as a
 * fraction of full scale, then reset. >= 1.0 means the stage clipped and the
 * preamp is too high. Diagnostic only.
 */
float audio_dsp_take_peak(void);

/**
 * Log the active cascade.
 *
 * @param tag    log tag to attribute the lines to.
 * @param level  an ESPHOME_LOG_LEVEL_* value. These boards run `level: INFO`,
 *               which is BELOW CONFIG, so a cascade logged only at CONFIG (i.e.
 *               only from dump_config) is invisible where it matters most --
 *               setup() logs it at INFO for that reason.
 */
void audio_dsp_log_cascade(const char *tag, int level);

}  // namespace airplay_receiver
}  // namespace esphome
