#pragma once
// airplay_receiver crypto module skeleton (place-holder).
//
// Holds HIPairing / Ed25519 / AES / SRP / the FairPlay key-exchange that
// decrypts the AirPlay 2 stream. Implemented by a later task. Self-contained.

#include <cstddef>

namespace esphome {
namespace airplay_receiver {

/**
 * Crypto shell: pairing + stream decryption context.
 * (PENDING: HIPairing, SRP, Ed25519, AES-CCM stream decryption, see
 * UPSTREAMING.md.)
 */
class CryptoModule {
 public:
  CryptoModule();
  ~CryptoModule();

  void setup();
  void loop();

  bool paired() const;

 private:
  bool paired_{false};
};

}  // namespace airplay_receiver
}  // namespace esphome
