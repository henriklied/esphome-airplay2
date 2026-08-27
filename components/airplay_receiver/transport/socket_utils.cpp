// airplay_receiver socket helpers (port of main/network/socket_utils.c).
#include "socket_utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>

#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_socket";

int socket_utils_bind_udp(uint16_t port, int recv_timeout_sec, int recvbuf_size,
                          uint16_t *bound_port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create UDP socket");
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
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create TCP socket");
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
