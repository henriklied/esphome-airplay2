#pragma once
// airplay_receiver decoder module skeleton (place-holder).
//
// Holds the ALAC / AAC decoder that turns the received audio packets into PCM
// for the ring buffer (decoder working set may live in PSRAM per
// AIRPLAY_DECODER_IN_PSRAM). Implemented by a later task. Self-contained.

#include <cstddef>

namespace esphome {
namespace airplay_receiver {

/**
 * Decoder shell: ALAC/AAC -> PCM. (PENDING: real decoder integration, see
 * UPSTREAMING.md.)
 */
class DecoderModule {
 public:
  DecoderModule();
  ~DecoderModule();

  void setup();
  void loop();

  bool is_decoding() const;

 private:
  bool decoding_{false};
};

}  // namespace airplay_receiver
}  // namespace esphome
