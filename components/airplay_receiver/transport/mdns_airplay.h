#pragma once
// airplay_receiver mDNS/Bonjour advertisement (port of main/network/mdns_airplay.c).
//
// Registers the AirPlay 2 service _airplay._tcp AND the AirPlay 1 / RAOP
// service _raop._tcp (both on port 7000) with the TXT records Apple's clients
// use for discovery + pairing (deviceid, features, model, pk, srcvers, vv,
// acl; raop: tp, md, sm, sf, et, cn, ss, sr, vn, pk, am, pi).
//
// IMPORTANT: ESPHome owns mDNS. This module must NOT call mdns_init() or
// mdns_hostname_set() — the MDNSComponent (running at AFTER_CONNECTION, ahead
// of AirPlayReceiver::setup()) already initializes the stack and sets the
// hostname. We only add our A/V services via the Espressif mdns_service_add()
// API once the stack is up.
//
// No heap is owned here beyond the caller-supplied buffers.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/**
 * Register the AirPlay A/V mDNS services (_airplay._tcp + _raop._tcp, port
 * 7000). Must be called after the network stack AND ESPHome's mDNS are up
 * (i.e. from AirPlayReceiver::setup(), which runs at AFTER_CONNECTION).
 *
 * @param device_name  UTF-8 user-facing AirPlay device name.
 * @param public_key   Ed25519 device long-term public key (32 bytes) from the
 *                     CryptoModule — advertised as the "pk" TXT record.
 * @param public_key_len  length of the public key.
 */
void mdns_airplay_init(const char *device_name, const uint8_t *public_key, size_t public_key_len);

}  // namespace airplay_receiver
}  // namespace esphome
