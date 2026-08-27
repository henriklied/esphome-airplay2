// airplay_receiver RTSP encrypted control channel (port of main/rtsp/rtsp_crypto.c).
#include "rtsp_crypto.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "../allocator.h"
#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_rtsp_crypto";

static int send_all(int socket, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t r = send(socket, data + sent, len - sent, 0);
    if (r <= 0) {
      return -1;
    }
    sent += (size_t)r;
  }
  return 0;
}

int rtsp_crypto_read_block(int socket, RtspConn *conn, uint8_t *buffer, size_t buffer_size) {
  if (conn == nullptr || conn->hap_session == nullptr || !conn->encrypted_mode) {
    return -1;
  }

  // Read the 2-byte little-endian length header. The socket has a receive
  // timeout for shutdown responsiveness; on EAGAIN/EWOULDBLOCK/EINTR keep
  // waiting so a partial length cannot desync the frame stream. Cancellation
  // is handled by shutdown(SHUT_RDWR) which makes recv return a real error.
  uint8_t len_buf[2];
  size_t received = 0;
  while (received < 2) {
    ssize_t r = recv(socket, len_buf + received, 2 - received, 0);
    if (r > 0) {
      received += (size_t)r;
      continue;
    }
    if (r == 0) {
      return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    return -1;
  }

  uint16_t block_len = (uint16_t)len_buf[0] | ((uint16_t)len_buf[1] << 8);
  if (block_len == 0 || block_len > AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX || block_len > buffer_size) {
    ESP_LOGE(TAG, "Invalid encrypted block length: %u", block_len);
    return -1;
  }

  size_t encrypted_len = block_len + 16;  // + Poly1305 tag
  uint8_t *encrypted = static_cast<uint8_t *>(airplay_alloc(encrypted_len, false));
  if (encrypted == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
    return -1;
  }

  received = 0;
  while (received < encrypted_len) {
    ssize_t r = recv(socket, encrypted + received, encrypted_len - received, 0);
    if (r > 0) {
      received += (size_t)r;
      continue;
    }
    if (r == 0) {
      airplay_free(encrypted);
      return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    airplay_free(encrypted);
    return -1;
  }

  size_t plaintext_len = 0;
  if (conn->crypto == nullptr || conn->crypto->session_decrypt(conn->hap_session, encrypted, encrypted_len,
                                                               len_buf, sizeof(len_buf), buffer,
                                                               &plaintext_len) != 0) {
    airplay_free(encrypted);
    ESP_LOGE(TAG, "Failed to decrypt control frame");
    return -1;
  }

  airplay_free(encrypted);
  return (int)plaintext_len;
}

int rtsp_crypto_write_frame(int socket, RtspConn *conn, const uint8_t *data, size_t data_len) {
  if (conn == nullptr || conn->hap_session == nullptr || !conn->encrypted_mode) {
    return -1;
  }

  size_t offset = 0;
  while (offset < data_len) {
    uint16_t block_len = (data_len - offset) > AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX
                            ? AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX
                            : (uint16_t)(data_len - offset);

    uint8_t len_buf[2];
    len_buf[0] = block_len & 0xFF;
    len_buf[1] = (uint8_t)((block_len >> 8) & 0xFF);

    size_t encrypted_len = block_len + 16;
    uint8_t *encrypted = static_cast<uint8_t *>(airplay_alloc(encrypted_len, false));
    if (encrypted == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
      return -1;
    }

    size_t ct_len = 0;
    if (conn->crypto == nullptr ||
        conn->crypto->session_encrypt(conn->hap_session, data + offset, block_len, len_buf,
                                      sizeof(len_buf), encrypted, &ct_len) != 0 ||
        ct_len != encrypted_len) {
      ESP_LOGE(TAG, "Failed to encrypt control frame");
      airplay_free(encrypted);
      return -1;
    }

    if (send_all(socket, len_buf, sizeof(len_buf)) != 0 || send_all(socket, encrypted, ct_len) != 0) {
      airplay_free(encrypted);
      return -1;
    }

    airplay_free(encrypted);
    offset += block_len;
  }

  return 0;
}

}  // namespace airplay_receiver
}  // namespace esphome
