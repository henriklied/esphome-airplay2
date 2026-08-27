#pragma once
// airplay_receiver audio module skeleton (place-holder).
//
// Holds the PCM ring buffer, the buffered/unbuffered stream receivers and the
// output backend. The PCM ring lives in PSRAM (AIRPLAY_RING_FRAMES frames).
// Implemented by a later task. Self-contained.

#include <cstddef>

namespace esphome {
namespace airplay_receiver {

/**
 * Audio shell: PCM ring + stream receivers + output stage.
 * (PENDING: realtime/buffered receivers, audio_buffer, output backend, see
 * UPSTREAMING.md.)
 */
class AudioModule {
 public:
  AudioModule();
  ~AudioModule();

  void setup();
  void loop();

  bool playing() const;

 private:
  bool playing_{false};
};

}  // namespace airplay_receiver
}  // namespace esphome
