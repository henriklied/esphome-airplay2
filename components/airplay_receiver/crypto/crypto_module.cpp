#include "crypto_module.h"

namespace esphome {
namespace airplay_receiver {

CryptoModule::CryptoModule() = default;
CryptoModule::~CryptoModule() = default;

void CryptoModule::setup() {
  // PENDING: real HIPairing setup.
}

void CryptoModule::loop() {
  // PENDING: real stream decryption.
}

bool CryptoModule::paired() const {
  return this->paired_;
}

}  // namespace airplay_receiver
}  // namespace esphome
