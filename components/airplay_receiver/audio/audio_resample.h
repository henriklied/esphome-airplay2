#pragma once
// airplay_receiver audio resampler.
//
// Port of upstream main/audio/audio_resample.c for the fixed-ratio, interleaved
// int16 stereo case (AirPlay sources are typically 44.1 kHz; the PCM5100 output
// may run at 44.1 or 48 kHz). The public API is preserved verbatim so the
// audio output engine can re-initialize / process / reset it as the source and
// output sample rates change. Internal float buffers and the resampler state
// are allocated through the airplay_* allocator.

#include <cstdbool>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/**
 * Initialize the audio resampler.
 *
 * @param input_rate   Source sample rate (e.g. 44100)
 * @param output_rate  Target sample rate (e.g. 48000)
 * @param channels     Number of channels (2 for stereo)
 * @return true on success
 */
bool audio_resample_init(uint32_t input_rate, uint32_t output_rate, int channels);

/**
 * Resample a block of interleaved int16_t PCM.
 *
 * @param in           Input samples (interleaved stereo int16)
 * @param in_frames    Number of input frames (samples per channel)
 * @param out          Output buffer (int16, must hold out_capacity frames)
 * @param out_capacity Maximum output frames the buffer can hold
 * @return Number of output frames written
 */
size_t audio_resample_process(const int16_t *in, size_t in_frames, int16_t *out, size_t out_capacity);

/**
 * @return true if resampling is active (rates differ)
 */
bool audio_resample_is_active(void);

/**
 * Reset resampler state (call on stream flush).
 */
void audio_resample_reset(void);

/**
 * Destroy the resampler and free resources.
 */
void audio_resample_destroy(void);

/**
 * Get the maximum number of output frames for a given input frame count.
 */
size_t audio_resample_max_output(size_t in_frames);

}  // namespace airplay_receiver
}  // namespace esphome
