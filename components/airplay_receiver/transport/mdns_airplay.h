#pragma once
// airplay_receiver mDNS/Bonjour advertisement (port of main/network/mdns_airplay.c).
//
// Registers the AirPlay 2 service _airplay._tcp on port 7000 with the TXT
// records iOS uses for discovery + pairing (deviceid, features, model, pk,
// srcvers, vv, acl). No heap is owned here beyond the caller-supplied buffers.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/**
 * Initialize mDNS and register the _airplay._tcp service (port 7000).
 *
 * @param device_name  UTF-8 user-facing AirPlay device name.
 * @param public_key   Ed25519 device long-term public key (32 bytes) from the
 *                     CryptoModule — advertised as the "pk" TXT record.
 * @param public_key_len  length of the public key.
 */
void mdns_airplay_init(const char *device_name, const uint8_t *public_key, size_t public_key_len);

}  // namespace airplay_receiver
}  // namespace esphome
