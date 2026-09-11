// airplay_receiver socket helpers (port of main/network/socket_utils.c).
#include "socket_utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_socket";

// ---------------------------------------------------------------------------
// Port reservations
// ---------------------------------------------------------------------------
//
// alloc_stream_port() uses a single transient socket to discover a free
// ephemeral port (bind :0, read the OS-assigned number), previously closes it
// and lets the audio engine re-acquire the SAME number later.  That close ->
// rebind window is a TOCTOU race: any other process on the LAN can grab the
// port between the advertisement and the audio engine bind, disconnecting the
// session.
//
// Instead we keep the reserved descriptor OPEN and hand it off verbatim to the
// matching socket_utils_bind_* call, so the advertised port is bound exactly
// once for the whole connection.  A small, mutex-guarded registry maps the
// reserved port -> held descriptor so the audio engine (which binds by port
// number) can consume it.  Unconsumed reservations (e.g. a timing port that is
// never bound) are released on connection teardown.
#define MAX_PORT_RESERVATIONS 8

typedef struct {
  uint16_t port;   // 0 = slot free
  int fd;          // held, already-bound descriptor
  bool udp;        // true = UDP socket, false = TCP listener
} PortReservation;

static PortReservation s_reservations[MAX_PORT_RESERVATIONS];
static portMUX_TYPE s_reservation_mux = portMUX_INITIALIZER_UNLOCKED;

static PortReservation *find_reservation(uint16_t port, bool udp) {
  for (int i = 0; i < MAX_PORT_RESERVATIONS; i++) {
    if (s_reservations[i].port == port && s_reservations[i].udp == udp) {
      return &s_reservations[i];
    }
  }
  return nullptr;
}

static PortReservation *find_free_slot() {
  for (int i = 0; i < MAX_PORT_RESERVATIONS; i++) {
    if (s_reservations[i].port == 0) {
      return &s_reservations[i];
    }
  }
  return nullptr;
}

int socket_utils_reserve_port(bool udp, uint16_t *bound_port) {
  if (bound_port == nullptr) {
    return -1;
  }

  uint16_t port = 0;
  int sock;
  if (udp) {
    sock = socket_utils_bind_udp(0, 0, 0, &port);
  } else {
    sock = socket_utils_bind_tcp_listener(0, 1, false, &port);
  }
  if (sock < 0 || port == 0) {
    if (sock >= 0) {
      close(sock);
    }
    return -1;
  }

  portENTER_CRITICAL(&s_reservation_mux);
  PortReservation *slot = find_free_slot();
  if (slot == nullptr) {
    portEXIT_CRITICAL(&s_reservation_mux);
    close(sock);
    ESP_LOGE(TAG, "Too many reserved ports; refusing reservation for %u", port);
    return -1;
  }
  slot->port = port;
  slot->fd = sock;
  slot->udp = udp;
  portEXIT_CRITICAL(&s_reservation_mux);

  *bound_port = port;
  return 0;
}

void socket_utils_release_reservation(uint16_t port) {
  if (port == 0) {
    return;
  }
  portENTER_CRITICAL(&s_reservation_mux);
  for (int i = 0; i < MAX_PORT_RESERVATIONS; i++) {
    if (s_reservations[i].port == port) {
      int fd = s_reservations[i].fd;
      s_reservations[i].port = 0;
      s_reservations[i].fd = -1;
      portEXIT_CRITICAL(&s_reservation_mux);
      if (fd >= 0) {
        close(fd);
      }
      return;
    }
  }
  portEXIT_CRITICAL(&s_reservation_mux);
}

int socket_utils_bind_udp(uint16_t port, int recv_timeout_sec, int recvbuf_size,
                          uint16_t *bound_port) {
  // If this port was reserved by socket_utils_reserve_port(), consume the
  // already-bound descriptor (bind exactly once) rather than rebinding it.
  if (port != 0) {
    portENTER_CRITICAL(&s_reservation_mux);
    PortReservation *res = find_reservation(port, /*udp=*/true);
    if (res != nullptr) {
      int reserved_fd = res->fd;
      res->port = 0;
      res->fd = -1;
      portEXIT_CRITICAL(&s_reservation_mux);
      if (recv_timeout_sec > 0) {
        struct timeval tv = {.tv_sec = recv_timeout_sec, .tv_usec = 0};
        setsockopt(reserved_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      }
      if (recvbuf_size > 0) {
        setsockopt(reserved_fd, SOL_SOCKET, SO_RCVBUF, &recvbuf_size, sizeof(recvbuf_size));
      }
      if (bound_port != nullptr) {
        *bound_port = port;
      }
      return reserved_fd;
    }
    portEXIT_CRITICAL(&s_reservation_mux);
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    // errno separates the two ways this fails: EMFILE/ENFILE means the LWIP
    // socket table (CONFIG_LWIP_MAX_SOCKETS) is full, ENOMEM means the netconn
    // or PCB pools are. They need different config bumps, so log which.
    ESP_LOGE(TAG, "Failed to create UDP socket: errno=%d", errno);
    return -1;
  }

  if (recv_timeout_sec > 0) {
    struct timeval tv = {.tv_sec = recv_timeout_sec, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  if (recvbuf_size > 0) {
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbuf_size, sizeof(recvbuf_size));
  }

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind UDP socket to port %u", port);
    close(sock);
    return -1;
  }

  if (bound_port != nullptr) {
    socklen_t addr_len = sizeof(addr);
    getsockname(sock, reinterpret_cast<struct sockaddr *>(&addr), &addr_len);
    *bound_port = ntohs(addr.sin_port);
  }

  return sock;
}

int socket_utils_bind_tcp_listener(uint16_t port, int backlog, bool nonblocking,
                                   uint16_t *bound_port) {
  // If this port was reserved by socket_utils_reserve_port(), consume the
  // already-bound, already-listening descriptor instead of rebinding.
  if (port != 0) {
    portENTER_CRITICAL(&s_reservation_mux);
    PortReservation *res = find_reservation(port, /*udp=*/false);
    if (res != nullptr) {
      int reserved_fd = res->fd;
      res->port = 0;
      res->fd = -1;
      portEXIT_CRITICAL(&s_reservation_mux);
      if (nonblocking) {
        int flags = fcntl(reserved_fd, F_GETFL, 0);
        fcntl(reserved_fd, F_SETFL, flags | O_NONBLOCK);
      }
      if (bound_port != nullptr) {
        *bound_port = port;
      }
      return reserved_fd;
    }
    portEXIT_CRITICAL(&s_reservation_mux);
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create TCP socket: errno=%d", errno);
    return -1;
  }

  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind TCP socket to port %u", port);
    close(sock);
    return -1;
  }

  if (listen(sock, backlog) < 0) {
    ESP_LOGE(TAG, "Failed to listen on TCP socket");
    close(sock);
    return -1;
  }

  if (nonblocking) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  }

  if (bound_port != nullptr) {
    socklen_t addr_len = sizeof(addr);
    getsockname(sock, reinterpret_cast<struct sockaddr *>(&addr), &addr_len);
    *bound_port = ntohs(addr.sin_port);
  }

  return sock;
}

}  // namespace airplay_receiver
}  // namespace esphome
