// airplay_receiver mDNS/Bonjour advertisement (port of main/network/mdns_airplay.c).
//
// Registers the AirPlay 2 service _airplay._tcp AND the AirPlay 1 / RAOP
// service _raop._tcp, both on port 7000. Apple's AirPlay picker keys off the
// _raop._tcp service to surface the device as an audio destination.
//
// ESPHome owns mDNS (MDNSComponent), so this module must NOT call mdns_init()
// or mdns_hostname_set() — doing so would double-initialize the stack and make
// ESPHome's MDNSComponent fail (ESP_ERR_INVALID_STATE -> lost _esphomelib._tcp
// discovery / .local OTA). We only run mdns_service_add(), which is safe once
// the stack is up. AirPlayReceiver::setup() runs at AFTER_CONNECTION, after
// the MDNSComponent.
#include "mdns_airplay.h"

#include <cstdio>
#include <cstring>

#include "esp_mac.h"
#include "esphome/core/log.h"
#include "mdns.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_mdns";

// AirPlay 2 feature flags (SupportsCoreUtilsPairingAndEncryption | SupportsHKPairingAndAccessControl |
// SupportsTransientPairing). Matches upstream AIRPLAY_FEATURES_HI/LO.
#define AIRPLAY_FEATURES_HI 0x1C340u
#define AIRPLAY_FEATURES_LO 0x405C4A00u
#define AIRPLAY_MODEL "AudioAccessory5,1"
#define AIRPLAY_FLAGS "0x4"           // audio receiver
#define AIRPLAY_PROTOCOL_VERSION "2"
#define AIRPLAY_SOURCE_VERSION "377.40.00"

void mdns_airplay_init(const char *device_name, const uint8_t *public_key, size_t public_key_len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  char device_id[18];
  snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);

  char features_str[32];
  snprintf(features_str, sizeof(features_str), "0x%X,0x%X", (unsigned)AIRPLAY_FEATURES_LO,
           (unsigned)AIRPLAY_FEATURES_HI);

  char pk_str[65] = {0};
  if (public_key != nullptr && public_key_len == 32) {
    for (size_t i = 0; i < 32; i++) {
      snprintf(pk_str + (size_t)i * 2, 3, "%02x", public_key[i]);
    }
  }

  const char *service_name = (device_name != nullptr && device_name[0] != '\0') ? device_name : "AirPlay2";

  // _airplay._tcp — AirPlay 2 discovery + pairing. Service instance name is the
  // bare device name (mirrors upstream mdns_airplay.c).
  mdns_txt_item_t airplay_txt[] = {
      {"deviceid", device_id},
      {"features", features_str},
      {"flags", AIRPLAY_FLAGS},
      {"model", AIRPLAY_MODEL},
      {"pk", pk_str},
      {"pi", "00000000-0000-0000-0000-000000000000"},
      {"srcvers", AIRPLAY_SOURCE_VERSION},
      {"vv", AIRPLAY_PROTOCOL_VERSION},
      {"acl", "0"},
  };
  esp_err_t err_svc = mdns_service_add(service_name, "_airplay", "_tcp", 7000, airplay_txt,
                                       sizeof(airplay_txt) / sizeof(airplay_txt[0]));
  if (err_svc != ESP_OK) {
    ESP_LOGW(TAG, "Failed to add _airplay._tcp service: %s", esp_err_to_name(err_svc));
  } else {
    ESP_LOGI(TAG, "Advertised _airplay._tcp on port 7000 (deviceid=%s, ft=%s)", device_id, features_str);
  }

  // _raop._tcp — what Apple's picker keys off as the audio destination.
  // The instance name MUST be <MAC uppercase hex, no colons>@<device name> so a
  // client can correlate this _raop record back to the _airplay device it just
  // paired with (upstream formats it the same way, e.g. DCB4D900A47C@Boston X90).
  // The TXT record is the AirPlay 2 dual-mode set: et=0,1,3,5 (none, RSA,
  // FairPlay, MFi-SAP) instead of the truncated 0,3 that advertised an
  // unimplemented FairPlay-only path; cn=0,1,2,3 lists AAC/AAC-ELD; vv=2 marks
  // it as an AirPlay 2 endpoint (otherwise the client reads it as AirPlay 1);
  // ft carries feature negotiation; vn=65537; md=0,2 advertises text+progress
  // but NOT artwork, which this receiver does not render.
  char raop_service_name[80];
  snprintf(raop_service_name, sizeof(raop_service_name), "%02X%02X%02X%02X%02X%02X@%s", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5], service_name);
  mdns_txt_item_t raop_txt[] = {
      {"am", AIRPLAY_MODEL},
      {"cn", "0,1,2,3"},              // codecs: PCM, ALAC, AAC, AAC-ELD
      {"da", "true"},                 // digest auth
      {"ek", "1"},                    // RSA key available
      {"et", "0,1,3,5"},              // encryption types (none, RSA, FairPlay, MFi-SAP)
      {"ft", features_str},           // feature negotiation (same as _airplay)
      {"md", "0,2"},                  // metadata types (text + progress; no artwork)
      {"pk", pk_str},
      {"sf", AIRPLAY_FLAGS},
      // Transport preference. UDP gets a realtime (type 96) stream, whose lost
      // packets can only be concealed: iOS ignores the RAOP 0x80 0xD5 resend
      // request on an AirPlay 2 PTP stream (measured: sent=3-10/s, recovered=0
      // over 77 consecutive seconds). TCP asks the sender for the buffered
      // (type 103) stream instead, which cannot drop packets.
      {"tp", "TCP"},
      {"vn", "65537"},
      {"vs", AIRPLAY_SOURCE_VERSION},
      {"vv", AIRPLAY_PROTOCOL_VERSION},
  };
  esp_err_t err_raop = mdns_service_add(raop_service_name, "_raop", "_tcp", 7000, raop_txt,
                                        sizeof(raop_txt) / sizeof(raop_txt[0]));
  if (err_raop != ESP_OK) {
    ESP_LOGW(TAG, "Failed to add _raop._tcp service: %s", esp_err_to_name(err_raop));
  } else {
    ESP_LOGI(TAG, "Advertised _raop._tcp '%s' on port 7000 (et=0,1,3,5 cn=0,1,2,3 vv=2)", raop_service_name);
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
