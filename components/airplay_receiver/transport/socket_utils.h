#pragma once
// airplay_receiver socket helpers.
//
// Thin wrappers around BSD sockets (bind UDP / TCP listener) ported from the
// upstream airplay-esp32 main/network/socket_utils.c. These own no heap that
// routes through airplay_* — they manage OS socket descriptors only.

#include <cstdint>

namespace esphome {
namespace airplay_receiver {

/**
 * Bind a UDP socket to `port` (0 = ephemeral). If `bound_port` is non-null the
 * OS-assigned port is written back after a successful bind.
 *
 * If `port` was previously reserved via socket_utils_reserve_port() as a UDP
 * socket, that already-bound descriptor is handed off (consumed) instead of
 * rebinding, so the advertised port is bound exactly once and cannot be stolen
 * in the SETUP -> RECORD window.
 *
 * @return  socket fd, or -1 on failure.
 */
int socket_utils_bind_udp(uint16_t port, int recv_timeout_sec, int recvbuf_size,
                          uint16_t *bound_port);

/**
 * Bind a TCP listener to `port` (0 = ephemeral) and call listen() with
 * `backlog`. If `nonblocking` is true the socket is put in O_NONBLOCK mode.
 * If `bound_port` is non-null the OS-assigned port is written back.
 *
 * If `port` was previously reserved via socket_utils_reserve_port() as a TCP
 * listener, that already-bound descriptor is handed off (consumed) instead of
 * rebinding, so the advertised port is bound exactly once.
 *
 * @return  listening socket fd, or -1 on failure.
 */
int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblocking,
                                   uint16_t *bound_port);

/**
 * Reserve an ephemeral UDP (udp=true) or TCP listener (udp=false) port by
 * binding a socket that stays open and remains owned by the caller until it is
 * consumed by the matching socket_utils_bind_* on the same port, or released
 * via socket_utils_release_reservation().
 *
 * This closes the TOCTOU gap in the SETUP path: the advertised port is held for
 * the whole session instead of being closed and re-acquired later.
 *
 * @param udp         true to reserve a UDP socket, false for a TCP listener.
 * @param bound_port  set to the reserved (OS-assigned) port number.
 * @return 0 on success (port reserved), -1 on failure.
 */
int socket_utils_reserve_port(bool udp, uint16_t *bound_port);

/**
 * Release a port reservation by port number (closes the held descriptor).
 * No-op if the port was not reserved (e.g. already consumed by a bind_* call).
 */
void socket_utils_release_reservation(uint16_t port);

}  // namespace airplay_receiver
}  // namespace esphome
