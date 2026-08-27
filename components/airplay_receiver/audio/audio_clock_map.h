#pragma once
// airplay_receiver audio_clock_map — anchor-based mapping between the RTP clock
// and the sender's network (wall-clock) time domain.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_clock_map.{c,h} (PR #130)
// into the ESPHome component. The upstream public API is preserved 1:1
// (function names, parameter lists and the struct layout are unchanged) and the
// whole module is wrapped in esphome::airplay_receiver.
//
// The mapping is anchored in the SENDER's clock domain, which is PTP for
// AirPlay 2 and NTP for AirPlay 1. The math is identical either way, so this
// module intentionally names no protocol: the CALLER converts local time into
// the domain the anchor arrived in (via the ptp_clock_* / ntp_clock_* helpers)
// before calling audio_clock_map_network_to_rtp(). See the header comment from
// upstream reproduced in the struct doc below.
//
// MEMORY POLICY: this module owns NO heap — the map lives in the caller's
// audio_clock_map_t storage. There is nothing to route through
// airplay_alloc/airplay_calloc/airplay_free (the ../allocator.h contract holds
// vacuously), and no malloc/calloc/new/free appears anywhere.

#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/* The anchor lives in the SENDER's clock domain, which is PTP for AirPlay 2
 * and NTP for AirPlay 1.  The mapping is identical either way, so nothing
 * here names a protocol; the caller converts local time into the domain the
 * anchor arrived in before calling audio_clock_map_network_to_rtp(). */
typedef struct {
  bool valid;
  uint32_t sample_rate;
  uint32_t anchor_rtp;
  uint64_t anchor_network_ns;
  int64_t playout_offset_ns;
} audio_clock_map_t;

void audio_clock_map_reset(audio_clock_map_t *map);
bool audio_clock_map_set(audio_clock_map_t *map, uint32_t sample_rate,
                         uint32_t anchor_rtp, uint64_t anchor_network_ns,
                         int64_t playout_offset_ns);
bool audio_clock_map_rtp_to_network(const audio_clock_map_t *map, uint32_t rtp,
                                    int64_t *network_ns);
bool audio_clock_map_network_to_rtp(const audio_clock_map_t *map,
                                    int64_t network_ns, uint32_t *rtp);

}  // namespace airplay_receiver
}  // namespace esphome
