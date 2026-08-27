#pragma once
// airplay_receiver NTP-style timing client (port of rbouteiller/airplay-esp32
// main/network/ntp_clock.c).
//
// Provides the time reference for the AirPlay 1 timing-client path: sends
// periodic timing requests (UDP) to the sender and derives the clock offset
// between the local playout clock and the remote sender clock from the
// responses (based on shairport-sync's timing implementation).
//
// This module owns NO heap: every buffer is file/static or on the FreeRTOS
// task stack, and the UDP socket is a plain OS descriptor. There is therefore
// nothing to route through airplay_alloc/airplay_calloc/airplay_free for now
// (the contract is upheld vacuously). The module does own a background FreeRTOS
// task and a UDP socket; both are released by ntp_clock_stop().

#include <cstdint>

#include "esp_err.h"

namespace esphome {
namespace airplay_receiver {

/**
 * Start the NTP timing client.
 *
 * Sends periodic timing requests to the remote sender and calculates the clock
 * offset from the responses. If already running against the same target the
 * existing client is left untouched; a different target first stops and
 * restarts the client.
 *
 * @param remote_ip    Remote IP address (network byte order)
 * @param remote_port  Remote timing port
 * @return ESP_OK on success
 */
esp_err_t ntp_clock_start_client(uint32_t remote_ip, uint16_t remote_port);

/**
 * Stop the NTP timing client and release the socket / FreeRTOS task.
 */
void ntp_clock_stop(void);

/**
 * Check whether the NTP timing client has a valid set of offset measurements.
 *
 * @return true once enough responses have been collected and processed
 */
bool ntp_clock_is_locked(void);

/**
 * Get the current offset from the local clock to the remote clock, in ns.
 *
 * remote_time = local_time + offset_ns
 */
int64_t ntp_clock_get_offset_ns(void);

}  // namespace airplay_receiver
}  // namespace esphome
