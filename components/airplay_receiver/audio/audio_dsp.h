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
//     never allocate, never take a lock, and never call libm on the per-sample
//     path.
//   * Every audio_dsp_set_*() recomputes coefficients (sin/cos/pow) and MUST be
//     called from a non-realtime context -- component setup() or the ESPHome
//     main loop -- with one exception: audio_dsp_set_sample_rate(), which the
//     playback task calls while reclocking, before the new stream is audible.
//     Writers serialise on a mutex the audio task never takes. They publish by
//     filling the inactive coefficient bank and then flipping a single index;
//     the audio task snapshots the active bank once per block.
//   * audio_dsp_set_volume_q15() is the other exception: it only stores a value
//     and is safe from any task.
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
 * True when the stage would alter the signal: enabled, and a preamp other than
 * 0 dB, at least one section, or any dynamic stage. Lets the caller skip the
 * whole conversion when nothing is configured.
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

// ---- Dynamic stages ------------------------------------------------------
// Processing order per stereo frame:
//   preamp -> biquad cascade -> loudness shelf -> + synthesized bass harmonics
//   -> stereo width + ambience -> crosstalk cancellation -> bass limiter
//   -> look-ahead limiter -> round and clamp
// The harmonic generator taps the signal after the preamp but BEFORE the
// cascade, so a protective high-pass cannot starve it of the very bass it is
// standing in for. Each stage is off until configured, and an off stage costs
// nothing per sample.

/**
 * Volume-following bass lift (loudness compensation). The ear loses bass
 * faster than mids as level drops, so the shelf boosts more the further the
 * volume sits below full scale:
 *
 *   boost_db = max_boost_db * clamp(-volume_db / range_db, 0, 1)
 *
 * where volume_db is the playback gain (audio_dsp_set_volume_q15). At full
 * volume the shelf is flat, so the limiter sees no extra bass where headroom
 * is scarcest.
 *
 * @param frequency_hz  shelf corner.
 * @param max_boost_db  lift at or below -range_db volume. 0 disables.
 * @param range_db      volume attenuation (positive dB) at which the full
 *                      boost is reached.
 */
void audio_dsp_set_loudness(float frequency_hz, float max_boost_db, float range_db);

/**
 * Report the playback gain the loudness stage follows. Safe from any task: it
 * only stores the value; audio_dsp_service() turns it into coefficients.
 *
 * @param volume_q15  linear gain, 32768 = unity, as applied by audio_output.
 */
void audio_dsp_set_volume_q15(int32_t volume_q15);

/**
 * Apply a pending loudness change. Call from the ESPHome main loop; it does
 * the libm work audio_dsp_set_volume_q15() deliberately avoids.
 */
void audio_dsp_service(void);

/// Loudness lift currently designed into the shelf, in dB.
float audio_dsp_get_loudness_boost_db(void);

/**
 * Psychoacoustic bass. Bass below `frequency_hz` -- where a small cabinet runs
 * out -- is isolated from the mono sum and replaced by its 2nd and 3rd
 * harmonics, which the speaker can play. The ear infers the missing
 * fundamental from the harmonic series. The harmonics are built from
 * Chebyshev polynomials of the envelope-normalised band, so their level tracks
 * the bass linearly instead of growing with drive like a distortion would.
 *
 * @param frequency_hz  upper edge of the band to synthesize from.
 * @param amount        harmonic level relative to the source band; 1.0 is
 *                      roughly equal energy. 0 disables.
 */
void audio_dsp_set_bass_enhancer(float frequency_hz, float amount);

/**
 * Mid/side stereo width above `frequency_hz`. 1.0 is unchanged, above widens,
 * below narrows (0 is mono above the corner). Bass stays at its original width
 * so the low end keeps its weight in the centre.
 */
void audio_dsp_set_stereo_width(float width, float frequency_hz);

/**
 * Centre lock for the width stage, 0 to 1. At 0 width boosts the plain side
 * signal, which also pushes off-centre and centre sources around. At 1 it
 * boosts only what is orthogonal to the dominant source (principal component
 * of the band above the width corner, tracked over ~30 ms), so vocals and
 * panned instruments keep their place and level while reverb and room sound
 * spread.
 */
void audio_dsp_set_centre_lock(float lock);

/**
 * Room-fill ambience: a small, damped feedback-delay-network reverb fed only
 * by the ambient component above the width corner (the part of the mix that
 * is not the dominant source), mixed back with decorrelated left and right
 * tails. A dry mono source feeds it nothing.
 *
 * @param amount      wet level, 0 (off) upward; 0.1-0.3 is subtle.
 * @param decay_ms    time for the tail to fall 60 dB.
 * @param damping_hz  low-pass inside the loop; lower is warmer and darker.
 */
void audio_dsp_set_ambience(float amount, float decay_ms, float damping_hz);

/**
 * Recursive crosstalk cancellation (RACE). Each output subtracts a delayed,
 * band-limited, attenuated copy of the OTHER channel's output:
 *
 *   out_l[n] = in_l[n] - amount * band(out_r[n - delay])
 *   out_r[n] = in_r[n] - amount * band(out_l[n - delay])
 *
 * The delay matches the extra path from each driver to the far ear, so the
 * subtraction arrives there with the crosstalk it cancels; the recursion
 * cancels the cancellation's own crosstalk in turn. Sound then images beyond
 * the cabinet. Within the band the side signal rises by up to
 * 1 / (1 - amount) and the mid falls by up to 1 / (1 + amount); outside it the
 * signal is untouched. Bass is excluded because cancelling it from closely
 * spaced drivers costs far more level than the image gains.
 *
 * @param amount     crossfeed gain, 0 (off) to below 1. Stable for any value
 *                   below 1: the band filter never exceeds unity.
 * @param delay_us   driver-to-far-ear path difference in microseconds; at least
 *                   one sample is used, since the recursion needs a past output.
 * @param low_hz     lower band edge (2nd-order high-pass).
 * @param high_hz    upper band edge (2nd-order low-pass) -- the head shadows
 *                   crosstalk above a few kHz anyway.
 */
void audio_dsp_set_crosstalk(float amount, float delay_us, float low_hz, float high_hz);

/**
 * Bass-band limiter. Splits at `frequency_hz` (Linkwitz-Riley 4th order, so the
 * bands sum back flat), runs the look-ahead limiter on the lows alone, and
 * delays the highs to match. Bass peaks -- the ones the loudness lift and the
 * harmonic generator push hardest -- are held below `threshold_db` without
 * ducking vocals the way the full-band limiter would. Adds one more look-ahead
 * of delay, included in audio_dsp_get_latency_frames().
 */
void audio_dsp_set_bass_limiter(bool enabled, float frequency_hz, float threshold_db, float release_ms);

/// Deepest bass-limiter gain reduction since the last call, in dB (<= 0).
float audio_dsp_take_bass_limiter_reduction_db(void);

/**
 * Look-ahead peak limiter on the final output, stereo-linked. Gain reduction
 * is guaranteed to be in place before a peak reaches the output (no
 * overshoot), and recovers with an exponential release. Adds
 * audio_dsp_get_latency_frames() of delay while enabled.
 *
 * @param threshold_db  ceiling in dBFS (negative).
 * @param release_ms    time constant of the gain recovery.
 */
void audio_dsp_set_limiter(bool enabled, float threshold_db, float release_ms);

/**
 * Frames of delay the stage adds to the output: one look-ahead per limiter in
 * the path, else 0. The playout clock adds this so AirPlay sync holds.
 */
uint32_t audio_dsp_get_latency_frames(void);

/**
 * Deepest limiter gain reduction since the last call, in dB (<= 0), then
 * reset. Diagnostic: a value that sits at several dB while the volume is low
 * means the loudness lift or the enhancer is set too hot.
 */
float audio_dsp_take_limiter_reduction_db(void);

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
