#include "decoder_module.h"

namespace esphome {
namespace airplay_receiver {

DecoderModule::DecoderModule() = default;
DecoderModule::~DecoderModule() = default;

void DecoderModule::setup() {
  // PENDING: real ALAC/AAC decoder setup.
}

void DecoderModule::loop() {
  // PENDING: real decode to PCM.
}

bool DecoderModule::is_decoding() const {
  return this->decoding_;
}

}  // namespace airplay_receiver
}  // namespace esphome
