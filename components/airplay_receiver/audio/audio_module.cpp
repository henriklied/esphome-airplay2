#include "audio_module.h"

namespace esphome {
namespace airplay_receiver {

AudioModule::AudioModule() = default;
AudioModule::~AudioModule() = default;

void AudioModule::setup() {
  // PENDING: real audio buffer / output setup.
}

void AudioModule::loop() {
  // PENDING: real stream receive + PCM ring feeding.
}

bool AudioModule::playing() const {
  return this->playing_;
}

}  // namespace airplay_receiver
}  // namespace esphome
