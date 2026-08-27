// airplay_receiver audio_clock_map — anchor-based mapping between the RTP clock
// and the sender's network (wall-clock) time domain.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_clock_map.c (PR #130) into
// the ESPHome component. The upstream logic is preserved exactly; the only
// change is that everything is wrapped in esphome::airplay_receiver. The module
// owns no heap and makes no network/PTP/NTP calls — it is pure integer math
// that converts between RTP timestamps and the sender's network-time domain.
// The caller is responsible for obtaining the playout offset (from
// ptp_clock_get_offset_ns() / ntp_clock_get_offset_ns()) and for expressing the
// anchor in the domain it actually arrived in (PTP for AirPlay 2, NTP for
// AirPlay 1) before invoking audio_clock_map_network_to_rtp().

#include "audio_clock_map.h"

#include <climits>

namespace esphome {
namespace airplay_receiver {

void audio_clock_map_reset(audio_clock_map_t *map) {
  if (!map) {
    return;
  }
  *map = (audio_clock_map_t) {0};
}

bool audio_clock_map_set(audio_clock_map_t *map, uint32_t sample_rate,
                         uint32_t anchor_rtp, uint64_t anchor_network_ns,
                         int64_t playout_offset_ns) {
  if (!map || sample_rate == 0U || anchor_network_ns > (uint64_t) INT64_MAX) {
    return false;
  }

  map->sample_rate = sample_rate;
  map->anchor_rtp = anchor_rtp;
  map->anchor_network_ns = anchor_network_ns;
  map->playout_offset_ns = playout_offset_ns;
  map->valid = true;
  return true;
}

bool audio_clock_map_rtp_to_network(const audio_clock_map_t *map, uint32_t rtp,
                                    int64_t *network_ns) {
  if (!map || !map->valid || !network_ns || map->sample_rate == 0U) {
    return false;
  }

  int64_t delta_samples = (int64_t) (int32_t) (rtp - map->anchor_rtp);
  int64_t delta_ns = (delta_samples * 1000000000LL) / map->sample_rate;
  *network_ns =
      (int64_t) map->anchor_network_ns + map->playout_offset_ns + delta_ns;
  return true;
}

bool audio_clock_map_network_to_rtp(const audio_clock_map_t *map,
                                    int64_t network_ns, uint32_t *rtp) {
  if (!map || !map->valid || !rtp || map->sample_rate == 0U) {
    return false;
  }

  int64_t base_ns = (int64_t) map->anchor_network_ns + map->playout_offset_ns;
  int64_t delta_ns = network_ns - base_ns;
  int64_t delta_samples = (delta_ns * map->sample_rate) / 1000000000LL;
  *rtp = map->anchor_rtp + (uint32_t) (int32_t) delta_samples;
  return true;
}

}  // namespace airplay_receiver
}  // namespace esphome
