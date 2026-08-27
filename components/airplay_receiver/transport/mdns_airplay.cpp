// airplay_receiver mDNS/Bonjour advertisement (port of main/network/mdns_airplay.c).
//
// AirPlay 2 only. The _raop._tcp (AirPlay 1 / RAOP) service is intentionally
// NOT registered — this port targets AirPlay 2 exclusively.
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

  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    // mdns_init returns ESP_ERR_INVALID_STATE if already initialized (e.g. the
    // ESPHome network component or a second call). Treat that as non-fatal.
    if (err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
      return;
    }
  }

  // Hostname: derive from the device name (upstream settings_device_name_to_hostname);
  // fall back to "airplay2". A bad/missing hostname must not crash the device,
  // so it is logged rather than ESP_ERROR_CHECK'd.
  char hostname[64] = "airplay2";
  if (device_name != nullptr && device_name[0] != '\0') {
    size_t o = 0;
    for (size_t i = 0; device_name[i] != '\0' && o < sizeof(hostname) - 1; i++) {
      char c = device_name[i];
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
        hostname[o++] = c;
      } else if (c >= 'A' && c <= 'Z') {
        hostname[o++] = (char) (c - 'A' + 'a');  // lowercase
      } else if (c == ' ' || c == '_') {
        hostname[o++] = '-';
      }
    }
    hostname[o] = '\0';
    if (o == 0) {
      strncpy(hostname, "airplay2", sizeof(hostname) - 1);
    }
  }
  esp_err_t err_host = mdns_hostname_set(hostname);
  if (err_host != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(err_host));
  }

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

  const char *service_name = (device_name != nullptr && device_name[0] != '\0') ? device_name : "AirPlay2";
  esp_err_t err_svc = mdns_service_add(service_name, "_airplay", "_tcp", 7000, airplay_txt,
                                        sizeof(airplay_txt) / sizeof(airplay_txt[0]));
  if (err_svc != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add _airplay._tcp service: %s", esp_err_to_name(err_svc));
  } else {
    ESP_LOGI(TAG, "Advertised _airplay._tcp on port 7000 (deviceid=%s, ft=%s)", device_id, features_str);
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
