#pragma once
// airplay_receiver AirPlay 2 transport/control layer (port of main/rtsp/* +
// main/network/mdns_airplay.c).
//
// Owns the _airplay._tcp mDNS advertisement and the RTSP server on port 7000.
// Every AirPlay 2 control method (ANNOUNCE/SETUP/RECORD/PAIR-SETUP/PAIR-VERIFY/
// GET_PARAMETER/SET_PARAMETER/TEARDOWN/FLUSH + OPTIONS/GET/POST/PAUSE/
// SETRATEANCHORTIME/SETPEERS) is served. PAIR-SETUP/PAIR-VERIFY drive the
// CryptoModule (HomeKit pairing: SRP-6a pair-setup, Ed25519+X25519+ChaCha20
// pair-verify). Streaming audio is NOT produced here — an event/callback
// interface hands the fully-configured stream to the audio engine.
//
// AirPlay 2 ONLY: AirPlay 1 RSA auth (rtsp_rsa.c) and AES-CBC audio encryption
// are deliberately not ported.
//
// All heap in the transport routes through airplay_* (realtime=false).

#include "esphome/core/component.h"

#include <cstdint>
#include <string>

#include "../crypto/crypto_module.h"
#include "rtsp_events.h"

namespace esphome {
namespace airplay_receiver {

// Feature flags advertised in mDNS + /info (AirPlay 2).
constexpr uint32_t AIRPLAY_FEATURES_HI = 0x1C340;   // SupportsCoreUtilsPairingAndEncryption | HKPairing | TransientPairing
constexpr uint32_t AIRPLAY_FEATURES_LO = 0x405C4A00;

/**
 * AirPlay 2 transport. Registered from AirPlayReceiver::setup(); the RTSP
 * server runs in a FreeRTOS task, one client task per accepted connection.
 */
class AirPlay2Transport {
 public:
  AirPlay2Transport();
  ~AirPlay2Transport();

  /**
   * Start mDNS advertisement + the RTSP server.
   *
   * @param crypto  CryptoModule used for pairing sessions (must be set up).
   * @param device_name  user-facing device name (mDNS service instance).
   */
  void setup(CryptoModule *crypto, const std::string &device_name);

  /// Poll hook (called from AirPlayReceiver::loop()); no busy work today.
  void loop();

  /// Stop the RTSP server + client tasks and unadvertise.
  void stop();

  bool started() const { return this->started_; }

  /// Audio-engine interface: register the transport event callback.
  int register_event_callback(TransportEventCallback callback, void *user_data);

  /// Current AirPlay stream type (96 realtime, 103 buffered), for accessors.
  int64_t current_stream_type() const;

 private:
  CryptoModule *crypto_{nullptr};
  std::string device_name_{"AirPlay2"};
  bool started_{false};
};

}  // namespace airplay_receiver
}  // namespace esphome
