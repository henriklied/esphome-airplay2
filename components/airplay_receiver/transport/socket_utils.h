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
 * @return  socket fd, or -1 on failure.
 */
int socket_utils_bind_udp(uint16_t port, int recv_timeout_sec, int recvbuf_size,
                          uint16_t *bound_port);

/**
 * Bind a TCP listener to `port` (0 = ephemeral) and call listen() with
 * `backlog`. If `nonblocking` is true the socket is put in O_NONBLOCK mode.
 * If `bound_port` is non-null the OS-assigned port is written back.
 *
 * @return  listening socket fd, or -1 on failure.
 */
int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblocking,
                                   uint16_t *bound_port);

}  // namespace airplay_receiver
}  // namespace esphome
