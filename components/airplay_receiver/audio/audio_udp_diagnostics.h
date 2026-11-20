#pragma once

#include <cstdint>

namespace esphome {
namespace airplay_receiver {

struct audio_udp_counters_t {
  uint32_t queued;
  uint32_t dropped;
};

/* Watch the realtime data socket's receive mailbox until stop(), which must
 * run before closing the descriptor. Counters start afresh with each socket. */
bool audio_udp_diagnostics_start(int socket_fd);
void audio_udp_diagnostics_stop();
audio_udp_counters_t audio_udp_diagnostics_counters();

}  // namespace airplay_receiver
}  // namespace esphome
