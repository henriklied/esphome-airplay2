// airplay_receiver AirPlay 2 transport/control layer (port of main/rtsp/*).
//
// AirPlay 2 ONLY. The RTSP server accepts connections on port 7000, parses the
// AirPlay 2 control methods, drives the CryptoModule for PAIR-SETUP/PAIR-VERIFY,
// and hands fully-configured streams to the audio engine via transport events.
#include "transport_module.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <string>

#include "esphome/core/log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../allocator.h"
#include "bplist.h"
#include "mdns_airplay.h"
#include "rtsp_conn.h"
#include "rtsp_crypto.h"
#include "rtsp_events.h"
#include "rtsp_message.h"
#include "socket_utils.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_transport";

#define RTSP_PORT 7000
#define RTSP_BUFFER_INITIAL 4096
#define RTSP_BUFFER_LARGE ((size_t)256 * 1024)
// Idle-header deadline (µs): if a client never delivers a complete
// \r\n\r\n-terminated RTSP head, close the slot instead of holding it forever.
#define RTSP_HEADER_IDLE_TIMEOUT_US ((int64_t)10 * 1000 * 1000)
#define RTSP_CLIENT_STACK_SIZE 8192
#define RTSP_SERVER_STACK_SIZE 4096
#define RTSP_EVENT_STACK_SIZE 4096
#define RTSP_AP2_AUDIO_BUFFER_SIZE (512 * 1024)        // buffered (type 103) TCP pre-fill
#define RTSP_AP2_REALTIME_AUDIO_BUFFER_SIZE (1 * 1024 * 1024)  // realtime (type 96) UDP

// ---------------------------------------------------------------------------
// Static transport state (one AirPlay2Transport instance per device; matches
// the upstream single-receiver model). Accessed from the FreeRTOS server,
// client and event-port tasks.
// ---------------------------------------------------------------------------
static CryptoModule *s_crypto = nullptr;

static int server_socket = -1;
static TaskHandle_t server_task_handle = nullptr;
static volatile bool server_running = false;

struct ClientSlot {
  RtspConn *conn = nullptr;
  TaskHandle_t task = nullptr;
  int socket = -1;
  volatile bool should_stop = false;
};
static ClientSlot clients[2];   // current + old (reconnect) slot
static int current_slot = 0;

// Forward-declared (defined below after method handlers).
int rtsp_dispatch(int socket, RtspConn *conn, const uint8_t *raw_request, size_t raw_len);

// Event port (AirPlay 2 server->client event socket) state.
static int event_client_socket = -1;
static int event_listen_socket = -1;
static TaskHandle_t event_task_handle = nullptr;
static volatile bool event_task_should_stop = false;

// Device info for /info + pairing (fixed buffer — keeps ALL transport heap on
// the airplay_* path; never a std::string).
#define AIRPLAY_DEVICE_NAME_MAX 80
static char s_device_name[AIRPLAY_DEVICE_NAME_MAX] = "AirPlay2";

// ===========================================================================
// TLV8 + small helpers
// ===========================================================================

static bool tlv8_find(const uint8_t *data, size_t len, uint8_t type, const uint8_t **value, size_t *value_len) {
  size_t pos = 0;
  while (pos + 2 <= len) {
    uint8_t t = data[pos];
    uint8_t l = data[pos + 1];
    if (pos + 2 + (size_t)l > len) {
      break;
    }
    if (t == type) {
      *value = data + pos + 2;
      *value_len = l;
      return true;
    }
    pos += 2 + (size_t)l;
  }
  return false;
}

// Bounded case-insensitive substring search within `len` bytes (avoids relying
// on the non-portable strcasestr).
static const char *ci_substr(const char *haystack, size_t len, const char *needle) {
  if (haystack == nullptr || needle == nullptr) {
    return nullptr;
  }
  size_t nlen = strlen(needle);
  if (nlen == 0) {
    return haystack;
  }
  if (len == 0) {
    len = strlen(haystack);
  }
  for (size_t p = 0; p + nlen <= len; ++p) {
    size_t i = 0;
    for (; i < nlen; ++i) {
      char a = haystack[p + i];
      char b = needle[i];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a != b) {
        break;
      }
    }
    if (i == nlen) {
      return haystack + p;
    }
  }
  return nullptr;
}

static bool request_uses_rtsp(const RtspRequest *req) {
  return req != nullptr && strncasecmp(req->protocol, "RTSP/", 5) == 0;
}

// Allocate a stream port by binding an ephemeral socket of the given type,
// recording the assigned port, then closing the probe socket. The audio engine
// later binds the real socket to the advertised port.
static uint16_t alloc_stream_port(bool udp) {
  uint16_t port = 0;
  int sock;
  if (udp) {
    sock = socket_utils_bind_udp(0, 0, 0, &port);
  } else {
    sock = socket_utils_bind_tcp_listener(0, 1, false, &port);
  }
  if (sock > 0) {
    close(sock);
  }
  return port;
}

static int create_event_socket(uint16_t *port) { return socket_utils_bind_tcp_listener(0, 1, false, port); }

// ===========================================================================
// Response body builders
// ===========================================================================

static void format_time_mmss(uint32_t seconds, char *out, size_t out_size) {
  uint32_t mins = seconds / 60;
  uint32_t secs = seconds % 60;
  snprintf(out, out_size, "%" PRIu32 ":%02" PRIu32, mins, secs);
}

// ===========================================================================
// Event port task — accepts the iOS event connection and tracks liveness.
// ===========================================================================
static void event_port_task(void *pv) {
  int listen_socket = (int)(intptr_t)pv;
  event_listen_socket = listen_socket;

  while (!event_task_should_stop && listen_socket >= 0) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client = accept(listen_socket, (struct sockaddr *)&client_addr, &addr_len);
    if (client < 0) {
      if (!event_task_should_stop) {
        ESP_LOGE(TAG, "Event port accept error: %d", errno);
      }
      break;
    }
    if (event_client_socket >= 0) {
      close(event_client_socket);
    }
    event_client_socket = client;
    ESP_LOGI(TAG, "Event client connected");
    transport_events_emit(TRANSPORT_EVENT_CLIENT_CONNECTED, nullptr);

    // Monitor the connection until it drops or we stop.
    while (!event_task_should_stop) {
      char buf[16];
      ssize_t n = recv(event_client_socket, buf, sizeof(buf), MSG_PEEK);
      if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
          break;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (event_client_socket >= 0) {
      close(event_client_socket);
      event_client_socket = -1;
    }
  }

  if (event_client_socket >= 0) {
    close(event_client_socket);
    event_client_socket = -1;
  }
  event_listen_socket = -1;
  event_task_handle = nullptr;
  vTaskDelete(nullptr);
}

static esp_err_t start_event_port_task(int listen_socket) {
  if (event_task_handle != nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  event_task_should_stop = false;
  BaseType_t ret = xTaskCreate(event_port_task, "airplay_event", RTSP_EVENT_STACK_SIZE,
                               (void *)(intptr_t)listen_socket, 5, &event_task_handle);
  if (ret != pdPASS) {
    event_task_handle = nullptr;
    ESP_LOGE(TAG, "Failed to create event port task");
    return ESP_FAIL;
  }
  return ESP_OK;
}

static void stop_event_port_task() {
  if (event_task_handle == nullptr) {
    return;
  }
  event_task_should_stop = true;
  if (event_client_socket >= 0) {
    shutdown(event_client_socket, SHUT_RDWR);
  }
  int timeout = 20;
  while (event_task_handle != nullptr && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (event_task_handle != nullptr) {
    ESP_LOGW(TAG, "Event port task did not exit within timeout");
  }
}

// ===========================================================================
// RTSP request buffer processing (framing + dispatch)
// ===========================================================================

static uint8_t *grow_buffer(uint8_t *old_buf, size_t old_size, size_t new_size, size_t data_len) {
  (void)old_size;
  uint8_t *new_buf = static_cast<uint8_t *>(airplay_alloc(new_size + 1, false));
  if (new_buf == nullptr) {
    return nullptr;
  }
  if (old_buf != nullptr && data_len > 0) {
    memcpy(new_buf, old_buf, data_len);
  }
  airplay_free(old_buf);
  return new_buf;
}

// Dispatch an RTSP/HTTP request to its registered handler (defined below).
int rtsp_dispatch(int socket, RtspConn *conn, const uint8_t *raw_request, size_t raw_len);

// Process fully-buffered RTSP requests (one or more per recv).
static void process_rtsp_buffer(ClientSlot *slot, uint8_t *buffer, size_t *buf_len) {
  while (*buf_len > 0 && !slot->should_stop) {
    const uint8_t *header_end = rtsp_find_header_end(buffer, *buf_len);
    if (header_end == nullptr) {
      break;
    }
    size_t header_len = (size_t)(header_end - buffer) + 4;
    char *header_str = static_cast<char *>(airplay_alloc(header_len + 1, false));
    if (header_str == nullptr) {
      *buf_len = 0;
      break;
    }
    memcpy(header_str, buffer, header_len);
    header_str[header_len] = '\0';

    int content_len = rtsp_parse_content_length(header_str);
    if (content_len < 0) {
      content_len = 0;
    }
    size_t total_len = header_len + (size_t)content_len;
    if (total_len > RTSP_BUFFER_LARGE || *buf_len < total_len) {
      airplay_free(header_str);
      if (total_len > RTSP_BUFFER_LARGE) {
        *buf_len = 0;
      }
      break;
    }

    // Null-terminate the message boundary so header parsers cannot over-read.
    uint8_t saved = buffer[total_len];
    buffer[total_len] = '\0';
    rtsp_dispatch(slot->socket, slot->conn, buffer, total_len);
    buffer[total_len] = saved;
    airplay_free(header_str);

    if (*buf_len > total_len) {
      memmove(buffer, buffer + total_len, *buf_len - total_len);
    }
    *buf_len -= total_len;
  }
}

// ===========================================================================
// Client task — one per accepted connection
// ===========================================================================
static void client_task(void *pv) {
  int slot_idx = (int)(intptr_t)pv;
  ClientSlot *slot = &clients[slot_idx];

  RtspConn *conn = rtsp_conn_create(s_crypto);
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Failed to create connection state");
    close(slot->socket);
    slot->socket = -1;
    slot->task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  slot->conn = conn;

  struct sockaddr_in peer_addr = {};
  socklen_t peer_len = sizeof(peer_addr);
  if (getpeername(slot->socket, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
    conn->client_ip = peer_addr.sin_addr.s_addr;
    ESP_LOGI(TAG, "Client IP: %u.%u.%u.%u", (unsigned int)(conn->client_ip & 0xFF),
             (unsigned int)((conn->client_ip >> 8) & 0xFF), (unsigned int)((conn->client_ip >> 16) & 0xFF),
             (unsigned int)((conn->client_ip >> 24) & 0xFF));
  }

  size_t buf_capacity = RTSP_BUFFER_INITIAL;
  // +1 spare byte so buffer[total_len]='\0' at the message boundary never steps
  // one past the allocation when a header+body exactly fills the buffer.
  uint8_t *buffer = static_cast<uint8_t *>(airplay_alloc(buf_capacity + 1, false));
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate buffer");
    rtsp_conn_free(conn);
    slot->conn = nullptr;
    close(slot->socket);
    slot->socket = -1;
    slot->task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  size_t buf_len = 0;
  // A client that connects and sends bytes that never form a complete
  // \r\n\r\n-terminated RTSP header (e.g. a TLS ClientHello, or raw binary)
  // would otherwise hold one of the two client slots forever. Force-close the
  // connection if no complete header has arrived within this window.
  const int64_t header_deadline_us = esp_timer_get_time() + RTSP_HEADER_IDLE_TIMEOUT_US;

  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(slot->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  int nodelay = 1;
  setsockopt(slot->socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  while (server_running && !slot->should_stop) {
    if (conn->encrypted_mode) {
      // Encrypted mode: read shaped length+payload frames.
      while (server_running && conn->encrypted_mode && !slot->should_stop) {
        if (buf_len >= buf_capacity - 1024) {
          size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE : buf_capacity * 2;
          if (new_cap > RTSP_BUFFER_LARGE) {
            goto cleanup;
          }
          uint8_t *nb = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
          if (nb == nullptr) {
            goto cleanup;
          }
          buffer = nb;
          buf_capacity = new_cap;
        }
        int block_len = rtsp_crypto_read_block(slot->socket, conn, buffer + buf_len, buf_capacity - buf_len);
        if (block_len <= 0) {
          if (slot->should_stop || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            goto cleanup;
          }
          continue;
        }
        buf_len += (size_t)block_len;
        process_rtsp_buffer(slot, buffer, &buf_len);
      }
      goto cleanup;
    }

    // Plain-text mode.
    if (buf_len >= buf_capacity - 1024) {
      size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE : buf_capacity * 2;
      if (new_cap > RTSP_BUFFER_LARGE) {
        break;
      }
      uint8_t *nb = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
      if (nb == nullptr) {
        break;
      }
      buffer = nb;
      buf_capacity = new_cap;
    }

    ssize_t recv_len = recv(slot->socket, buffer + buf_len, buf_capacity - buf_len, 0);
    if (recv_len <= 0) {
      if (recv_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        // No bytes this 1 s window. If we still do not hold a complete RTSP
        // header and the deadline has passed, close the slot (a silent or
        // non-RTSP client must not hold it forever).
        if (buf_len > 0 && esp_timer_get_time() > header_deadline_us) {
          ESP_LOGW(TAG, "Client slot %d: no complete RTSP header within timeout; closing", slot_idx);
          break;
        }
        continue;
      }
      break;
    }
    buf_len += (size_t)recv_len;
    process_rtsp_buffer(slot, buffer, &buf_len);
    // If the client only ever sends non-RTSP bytes, buf_len grows without a
    // header appearing; once the deadline passes, stop.
    if (buf_len > 0 && esp_timer_get_time() > header_deadline_us) {
      ESP_LOGW(TAG, "Client slot %d: header never completed within timeout; closing", slot_idx);
      break;
    }
  }

cleanup:
  ESP_LOGI(TAG, "Client slot %d disconnected", slot_idx);
  airplay_free(buffer);
  close(slot->socket);
  slot->socket = -1;

  transport_events_emit(TRANSPORT_EVENT_DISCONNECTED, nullptr);
  stop_event_port_task();

  rtsp_conn_free(conn);
  slot->conn = nullptr;
  slot->socket = -1;
  slot->task = nullptr;
  slot->should_stop = false;

  vTaskDelete(nullptr);
}

// ===========================================================================
// RTSP method handlers (AirPlay 2)
// ===========================================================================

static void handle_options(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw,
                           size_t raw_len) {
  (void)raw;
  (void)raw_len;
  const char *public_methods =
      "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, "
      "OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, "
      "SETRATEANCHORTIME\r\n";
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, public_methods, nullptr, 0);
}

static void handle_get(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  if (strcmp(req->path, "/info") == 0) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_id[18];
    snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);

    uint64_t features = ((uint64_t)AIRPLAY_FEATURES_HI << 32) | AIRPLAY_FEATURES_LO;
    const uint8_t *pk = (s_crypto != nullptr) ? s_crypto->device_public_key() : nullptr;
    if (pk == nullptr) {
      static const uint8_t zero_pk[32] = {};
      pk = zero_pk;
    }

    // Per-call (not static): there are two client tasks sharing this handler,
    // and a shared buffer would let a concurrent GET /info clobber the body
    // being sent on the other slot. 1024 B on the 8 KiB client-task stack is
    // fine.
    uint8_t body[1024];
    size_t body_len = bplist_build_info_response(body, sizeof(body), device_id, s_device_name, pk, 32,
                                                 features, 2);
    if (body_len == 0) {
      ESP_LOGE(TAG, "Failed to build binary /info response");
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, nullptr, nullptr, 0);
      return;
    }
    rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                       "Content-Type: application/x-apple-binary-plist\r\n", (const char *)body, body_len);
    return;
  }

  ESP_LOGW(TAG, "Unknown GET path: %s", req->path);
  rtsp_send_response(socket, conn, 404, "Not Found", req->cseq, "Content-Type: text/plain\r\n", "Not Found", 9);
}

// POST /pair-setup — drive CryptoModule SRP-6a pair-setup (M1/M3/M5).
// POST /pair-verify — drive CryptoModule Ed25519+X25519 pair-verify (M1/M3).
static void handle_post(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  if (strstr(req->path, "/pair-setup")) {
    conn->protocol_version = 2;
    if (conn->hap_session == nullptr && s_crypto != nullptr) {
      conn->hap_session = s_crypto->create_session();
    }
    if (conn->hap_session == nullptr) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, nullptr, nullptr, 0);
      return;
    }

    size_t response_cap = 2048;
    uint8_t *response = static_cast<uint8_t *>(airplay_alloc(response_cap, false));
    if (response == nullptr) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, nullptr, nullptr, 0);
      return;
    }
    size_t response_len = 0;
    int err = -1;

    if (body != nullptr && body_len > 0) {
      const uint8_t *state = nullptr;
      size_t state_len = 0;
      if (tlv8_find(body, body_len, 0x06, &state, &state_len) && state_len == 1) {  // TLV8_STATE
        switch (state[0]) {
          case 1:
            err = s_crypto->pair_setup_m1(conn->hap_session, body, body_len, response, response_cap, &response_len);
            break;
          case 3:
            err = s_crypto->pair_setup_m3(conn->hap_session, body, body_len, response, response_cap, &response_len);
            break;
          case 5:
            err = s_crypto->pair_setup_m5(conn->hap_session, body, body_len, response, response_cap, &response_len);
            // Successful M5 (device added) enables the encrypted control channel.
            if (err == 0) {
              conn->encrypted_mode = true;
            }
            break;
          default:
            break;
        }
      }
    }

    if (err == 0 && response_len > 0) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/octet-stream\r\n",
                         (const char *)response, response_len);
    } else {
      ESP_LOGE(TAG, "Pair-setup failed: err=%d", err);
      static const uint8_t error_response[] = {0x06, 0x01, 0x02, 0x07, 0x01, 0x02};
      rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/octet-stream\r\n",
                         (const char *)error_response, sizeof(error_response));
    }
    airplay_free(response);
    return;
  }

  if (strstr(req->path, "/pair-verify")) {
    conn->protocol_version = 2;
    if (conn->hap_session == nullptr && s_crypto != nullptr) {
      conn->hap_session = s_crypto->create_session();
    }
    if (conn->hap_session == nullptr) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, nullptr, nullptr, 0);
      return;
    }

    size_t response_cap = 1024;
    uint8_t *response = static_cast<uint8_t *>(airplay_alloc(response_cap, false));
    if (response == nullptr) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, nullptr, nullptr, 0);
      return;
    }
    size_t response_len = 0;
    int err = -1;

    if (body != nullptr && body_len > 0) {
      const uint8_t *state = nullptr;
      size_t state_len = 0;
      if (tlv8_find(body, body_len, 0x06, &state, &state_len) && state_len == 1) {  // TLV8_STATE
        if (state[0] == 0x01) {
          err = s_crypto->pair_verify_m1(conn->hap_session, body, body_len, response, response_cap, &response_len);
        } else if (state[0] == 0x03) {
          err = s_crypto->pair_verify_m3(conn->hap_session, body, body_len, response, response_cap, &response_len);
          // Successful TLV8 M3 establishes the encrypted RTSP control channel.
          if (err == 0) {
            conn->encrypted_mode = true;
            ESP_LOGI(TAG, "RTSP encryption enabled (TLV8 pair-verify)");
          }
        }
      } else {
        // Raw (non-TLV8) pair-verify: used for audio-key exchange, not the RTSP
        // channel. The CryptoModule tracks its internal state; we try M1 first,
        // then M3 as a fallback for the case where M1 was already consumed.
        err = s_crypto->pair_verify_m1_raw(conn->hap_session, body, body_len, response, response_cap, &response_len);
        if (err != 0 || response_len == 0) {
          err = s_crypto->pair_verify_m3_raw(conn->hap_session, body, body_len, response, response_cap, &response_len);
        }
      }
    }

    if (err == 0 && response_len > 0) {
      rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/octet-stream\r\n",
                         (const char *)response, response_len);
    } else {
      ESP_LOGE(TAG, "Pair-verify failed, err=%d", err);
      rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/octet-stream\r\n",
                         "\x06\x01\x04\x07\x01\x02", 6);
    }
    airplay_free(response);
    return;
  }

  if (strstr(req->path, "/fp-setup")) {
    // AirPlay 1 / FairPlay handshake — not ported. Answer with a minimal bloom.
    rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/octet-stream\r\n", "\x00", 1);
    return;
  }

  if (strstr(req->path, "/command")) {
    if (body != nullptr && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t cmd_type = 0;
      if (bplist_find_int(body, body_len, "type", &cmd_type)) {
        ESP_LOGI(TAG, "/command type=%lld", (long long)cmd_type);
      }
    }
    rtsp_send_ok(socket, conn, req->cseq);
    return;
  }

  if (strstr(req->path, "/feedback")) {
    if (body != nullptr && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value = 0;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI(TAG, "/feedback networkTimeSecs=%lld", (long long)value);
      }
    }
    if (conn->stream_type == 103) {
      uint8_t response[128];
      size_t response_len = bplist_build_feedback_response(response, sizeof(response), conn->stream_type, 44100.0);
      if (response_len > 0) {
        rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/x-apple-binary-plist\r\n",
                           (const char *)response, response_len);
      } else {
        rtsp_send_ok(socket, conn, req->cseq);
      }
    } else {
      rtsp_send_ok(socket, conn, req->cseq);
    }
    return;
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

// Parse an SDP body to extract codec/rate (AirPlay 2 ANNOUNCE carries codec
// info). RSA/AES-CBC (AirPlay 1) is deliberately not parsed.
static void parse_sdp_airplay2(RtspConn *conn, const char *sdp, size_t len) {
  (void)len;
  conn->sample_rate = 44100;
  conn->channels = 2;
  conn->bits_per_sample = 16;
  strncpy(conn->codec, "AppleLossless", sizeof(conn->codec) - 1);

  const char *rtpmap = strstr(sdp, "a=rtpmap:");
  if (rtpmap != nullptr) {
    char codec[32];
    if (sscanf(rtpmap, "a=rtpmap:%*d %31s", codec) == 1) {
      char *slash = strchr(codec, '/');
      if (slash != nullptr) {
        *slash = '\0';
        int sr = 0, ch = 0;
        if (sscanf(slash + 1, "%d/%d", &sr, &ch) >= 1) {
          if (sr > 0) {
            conn->sample_rate = sr;
          }
          if (ch > 0) {
            conn->channels = ch;
          }
        }
      }
      strncpy(conn->codec, codec, sizeof(conn->codec) - 1);
    }
  }
}

static void handle_announce(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  ESP_LOGI(TAG, "ANNOUNCE (%s, %zu bytes)", req->content_type, req->body_len);
  conn->protocol_version = 2;
  if (req->body != nullptr && req->body_len > 0) {
    parse_sdp_airplay2(conn, (const char *)req->body, req->body_len);
    transport_set_stream_type(conn->stream_type);
  }
  (void)raw;
  (void)raw_len;
  rtsp_send_ok(socket, conn, req->cseq);
}

// SETUP: initial session (no streams -> eventPort/timingPort) or stream SETUP
// (streams[] -> dataPort/controlPort + audio config handed to the engine).
static void handle_setup(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  bool is_bplist = strstr(req->content_type, "application/x-apple-binary-plist") != nullptr;
  bool request_has_streams = false;
  size_t stream_count = 0;
  if (body != nullptr && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    if (bplist_get_streams_count(body, body_len, &stream_count)) {
      request_has_streams = true;
    }
  }

  bool is_v1_transport_setup =
      !request_has_streams && (ci_substr((const char *)raw, raw_len, "Transport:") != nullptr);

  // Per-stream dict fields carried into TransportAudioConfig for the audio
  // engine (codec, samples-per-frame, realtime playout latency). Function
  // scope so they survive into the stream SETUP block below.
  int64_t codec_type = 0;  // bplist "ct": 2=ALAC, 4=AAC, 8=AAC-ELD
  int spf = 0;             // bplist "spf": samples per frame
  int latency_min = 0;     // bplist "latencyMin": realtime playout latency samples

  // AirPlay 2 stream path.
  if (body != nullptr && body_len > 0 && is_bplist && request_has_streams) {
    conn->protocol_version = 2;
    for (size_t i = 0; i < stream_count; i++) {
      int64_t stream_type = -1;
      size_t ekey_len = 0, eiv_len = 0, shk_len = 0;
      if (bplist_get_stream_info(body, body_len, i, &stream_type, &ekey_len, &eiv_len, &shk_len)) {
        conn->stream_type = stream_type;
        transport_set_stream_type(stream_type);
      }

      // Stream dict keys: ct (codec type), sr (sample rate), spf, latencyMin,
      // controlPort. Carry the codec/framing/latency into TransportAudioConfig
      // so the audio engine decodes the right codec and gates/syncs correctly.
      bplist_kv_info_t kv[16];
      size_t kv_count = 0;
      if (bplist_get_stream_kv_info(body, body_len, i, kv, 16, &kv_count)) {
        for (size_t k = 0; k < kv_count; k++) {
          if (kv[k].value_type == BPLIST_VALUE_INT) {
            if (strcmp(kv[k].key, "ct") == 0) {
              codec_type = (int64_t)kv[k].int_value;
            } else if (strcmp(kv[k].key, "sr") == 0) {
              conn->sample_rate = (int)kv[k].int_value;
            } else if (strcmp(kv[k].key, "spf") == 0) {
              spf = (int)kv[k].int_value;
            } else if (strcmp(kv[k].key, "latencyMin") == 0) {
              latency_min = (int)kv[k].int_value;
            } else if (strcmp(kv[k].key, "controlPort") == 0) {
              conn->client_control_port = (uint16_t)kv[k].int_value;
            }
          }
        }
      }
    }
  }

  // Extract per-stream crypto key material (ekey/eiv/shk).
  TransportAudioConfig audio{};
  audio.stream_type = conn->stream_type > 0 ? conn->stream_type : 96;
  audio.sample_rate = conn->sample_rate;
  audio.channels = (conn->channels != 0) ? conn->channels : 2;
  audio.bits_per_sample = 16;
  audio.event_port = conn->event_port;

  if (body != nullptr && body_len > 0) {
    uint8_t ekey_encrypted[64] = {};
    size_t ekey_len = 0;
    uint8_t eiv[16] = {};
    size_t eiv_len = 0;
    uint8_t shk[32] = {};
    size_t shk_len = 0;

    int64_t crypto_stream_type = conn->stream_type > 0 ? conn->stream_type : 96;
    bool has_stream_crypto = bplist_find_stream_crypto(body, body_len, crypto_stream_type, ekey_encrypted,
                                                       sizeof(ekey_encrypted), &ekey_len, eiv, sizeof(eiv), &eiv_len,
                                                       shk, sizeof(shk), &shk_len);
    if (!has_stream_crypto || (ekey_len == 0 && shk_len == 0)) {
      bplist_find_data_deep(body, body_len, "ekey", ekey_encrypted, sizeof(ekey_encrypted), &ekey_len);
      bplist_find_data_deep(body, body_len, "eiv", eiv, sizeof(eiv), &eiv_len);
      bplist_find_data_deep(body, body_len, "shk", shk, sizeof(shk), &shk_len);
    }

    if (shk_len >= 16 && shk_len <= sizeof(audio.shk)) {
      audio.has_shk = true;
      memcpy(audio.shk, shk, shk_len);
      audio.shk_len = shk_len;
    }
    if (ekey_len > 0 && ekey_len <= sizeof(audio.ekey)) {
      audio.has_ekey = true;
      memcpy(audio.ekey, ekey_encrypted, ekey_len);
      audio.ekey_len = ekey_len;
    }
    if (eiv_len > 0 && eiv_len <= sizeof(audio.eiv)) {
      audio.has_eiv = true;
      memcpy(audio.eiv, eiv, eiv_len);
      audio.eiv_len = eiv_len;
    }

    // Resolve the stream key exactly as upstream rtsp_handlers.c does: prefer
    // shk, else ChaCha20-Poly1305-decrypt ekey with the pair-verify shared
    // secret, else HKDF-derive the audio key. `conn->hap_session` supplies the
    // shared secret; configure_audio_encryption handles the shk path even when
    // the session is not yet established.
    if (s_crypto != nullptr) {
      AudioEncrypt enc{};
      if (s_crypto->configure_audio_encryption(conn->hap_session, ekey_encrypted, ekey_len, eiv, eiv_len,
                                               shk, shk_len, &enc) == 0 &&
          enc.type == AudioEncryptType::CHACHA20_POLY1305 && enc.key_len > 0) {
        audio.has_encrypt = true;
        audio.encrypt_key_len = std::min<size_t>(enc.key_len, sizeof(audio.encrypt_key));
        memcpy(audio.encrypt_key, enc.key, audio.encrypt_key_len);
      }
    }
  }

  // Create the event port for AirPlay 2.
  if (!is_v1_transport_setup && conn->event_port == 0) {
    conn->event_socket = create_event_socket(&conn->event_port);
    if (conn->event_socket >= 0) {
      if (start_event_port_task(conn->event_socket) == ESP_OK) {
        ESP_LOGI(TAG, "SETUP: Created event port %u", conn->event_port);
      } else {
        close(conn->event_socket);
        conn->event_socket = -1;
        conn->event_port = 0;
      }
    }
  }
  audio.event_port = conn->event_port;

  // Initial SETUP (no streams) — respond with eventPort/timingPort.
  if (!request_has_streams) {
    if (is_v1_transport_setup) {
      // AirPlay 1 stream setup — not the target of this port, answer OK.
      conn->protocol_version = 1;
      rtsp_send_ok(socket, conn, req->cseq);
      return;
    }
    ESP_LOGI(TAG, "SETUP: Initial connection setup (no streams)");
    if (is_bplist) {
      uint8_t plist_body[128];
      size_t plist_len = bplist_build_initial_setup(plist_body, sizeof(plist_body), conn->event_port);
      if (plist_len == 0) {
        rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, nullptr, nullptr, 0);
        return;
      }
      rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/x-apple-binary-plist\r\n",
                         (const char *)plist_body, plist_len);
    } else {
      rtsp_send_ok(socket, conn, req->cseq);
    }
    return;
  }

  // Stream SETUP.
  int64_t stream_type = conn->stream_type > 0 ? conn->stream_type : 96;
  bool buffered = (stream_type == 103);

  if (conn->data_port == 0) {
    conn->data_port = alloc_stream_port(true);
  }
  if (conn->control_port == 0) {
    conn->control_port = alloc_stream_port(true);
  }
  if (conn->timing_port == 0) {
    conn->timing_port = alloc_stream_port(true);
  }
  if (buffered && conn->buffered_port == 0) {
    conn->buffered_port = alloc_stream_port(false);
  }

  audio.stream_type = stream_type;
  audio.data_port = conn->data_port;
  audio.control_port = conn->control_port;
  audio.timing_port = conn->timing_port;
  audio.buffered_port = conn->buffered_port;
  audio.codec_type = codec_type;
  audio.frame_size = spf;
  // Realtime (type 96) plays out latencyMin samples after the anchor; buffered
  // (type 103) plays at the anchor (0). 11025 = 250 ms default (upstream).
  audio.playout_latency_samples = buffered ? 0 : (latency_min > 0 ? latency_min : 11025);
  audio.audio_buffer_size = buffered ? RTSP_AP2_AUDIO_BUFFER_SIZE : RTSP_AP2_REALTIME_AUDIO_BUFFER_SIZE;

  // Hand the fully-configured stream to the audio engine.
  TransportEventData data{};
  data.audio = audio;
  transport_events_emit(TRANSPORT_EVENT_AUDIO_CONFIGURED, &data);

  if (is_bplist) {
    uint8_t plist_body[256];
    // Advertise the TCP port the sender must connect to: buffered streams use
    // buffered_port, realtime uses the UDP data_port.
    uint16_t ad_port = buffered ? conn->buffered_port : conn->data_port;
    size_t plist_len = bplist_build_stream_setup(plist_body, sizeof(plist_body), stream_type, ad_port,
                                                 conn->control_port,
                                                 buffered ? RTSP_AP2_AUDIO_BUFFER_SIZE
                                                          : RTSP_AP2_REALTIME_AUDIO_BUFFER_SIZE);
    if (plist_len == 0) {
      rtsp_send_response(socket, conn, 500, "Internal Error", req->cseq, nullptr, nullptr, 0);
      return;
    }
    ESP_LOGI(TAG, "SETUP response: type=%lld dataPort=%u controlPort=%u", (long long)stream_type, conn->data_port,
             conn->control_port);
    rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: application/x-apple-binary-plist\r\n",
                       (const char *)plist_body, plist_len);
  } else {
    rtsp_send_ok(socket, conn, req->cseq);
  }

  conn->stream_active = true;
  conn->stream_paused = false;
  transport_events_emit(TRANSPORT_EVENT_PLAYING, nullptr);
  (void)raw;
}

static void handle_record(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  ESP_LOGI(TAG, "RECORD received (stream_paused was %d)", conn->stream_paused);
  conn->stream_paused = false;
  conn->stream_active = true;
  transport_events_emit(TRANSPORT_EVENT_PLAYING, nullptr);

  char headers[128];
  uint32_t latency_samples = 0;
  snprintf(headers, sizeof(headers), "Audio-Latency: %" PRIu32 "\r\nAudio-Jack-Status: connected\r\n",
           latency_samples);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, nullptr, 0);
}

#define DMAP_MAX_NESTING 8

static void parse_dmap_metadata(const uint8_t *data, size_t len, TransportMetadata *meta, int depth) {
  size_t pos = 0;
  if (depth > DMAP_MAX_NESTING) {
    return;
  }
  while (pos + 8 <= len) {
    char tag[5] = {0};
    memcpy(tag, data + pos, 4);
    pos += 4;
    uint32_t item_len = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos + 1] << 16) |
                        ((uint32_t)data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (item_len > len - pos) {
      break;
    }
    if (strcmp(tag, "minm") == 0 && item_len > 0) {
      size_t copy_len = std::min(item_len, (uint32_t)(TRANSPORT_METADATA_STRING_MAX - 1));
      memcpy(meta->title, data + pos, copy_len);
      meta->title[copy_len] = '\0';
    } else if (strcmp(tag, "asar") == 0 && item_len > 0) {
      size_t copy_len = std::min(item_len, (uint32_t)(TRANSPORT_METADATA_STRING_MAX - 1));
      memcpy(meta->artist, data + pos, copy_len);
      meta->artist[copy_len] = '\0';
    } else if (strcmp(tag, "asal") == 0 && item_len > 0) {
      size_t copy_len = std::min(item_len, (uint32_t)(TRANSPORT_METADATA_STRING_MAX - 1));
      memcpy(meta->album, data + pos, copy_len);
      meta->album[copy_len] = '\0';
    } else if (strcmp(tag, "asgn") == 0 && item_len > 0) {
      size_t copy_len = std::min(item_len, (uint32_t)(TRANSPORT_METADATA_STRING_MAX - 1));
      memcpy(meta->genre, data + pos, copy_len);
      meta->genre[copy_len] = '\0';
    } else if (strcmp(tag, "mlit") == 0 || strcmp(tag, "cmst") == 0 || strcmp(tag, "mdst") == 0) {
      parse_dmap_metadata(data + pos, item_len, meta, depth + 1);
    }
    pos += item_len;
  }
}

static void parse_progress(const char *progress_str, uint32_t sample_rate, TransportMetadata *meta) {
  uint64_t start = 0, current = 0, end = 0;
  if (sscanf(progress_str, "%" SCNu64 "/%" SCNu64 "/%" SCNu64, &start, &current, &end) == 3) {
    if (sample_rate == 0) {
      sample_rate = 44100;
    }
    meta->position_secs = (uint32_t)((current - start) / sample_rate);
    meta->duration_secs = (uint32_t)((end - start) / sample_rate);
    char pos_str[16], dur_str[16];
    format_time_mmss(meta->position_secs, pos_str, sizeof(pos_str));
    format_time_mmss(meta->duration_secs, dur_str, sizeof(dur_str));
    ESP_LOGI(TAG, "Progress: %s / %s (raw: %" PRIu64 "/%" PRIu64 "/%" PRIu64 ")", pos_str, dur_str, start, current,
             end);
  }
}

static void handle_set_parameter(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw,
                                 size_t raw_len) {
  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  TransportEventData event_data{};
  bool has_metadata = false;

  const char *progress_hdr = ci_substr((const char *)raw, raw_len, "progress:");
  if (progress_hdr != nullptr) {
    const char *line_end = strstr(progress_hdr, "\r\n");
    if (line_end != nullptr) {
      size_t val_start = 9;
      while (progress_hdr[val_start] == ' ') {
        val_start++;
      }
      char progress_val[64];
      size_t val_len = (size_t)(line_end - (progress_hdr + val_start));
      if (val_len < sizeof(progress_val)) {
        memcpy(progress_val, progress_hdr + val_start, val_len);
        progress_val[val_len] = '\0';
        parse_progress(progress_val, 44100, &event_data.metadata);
        has_metadata = true;
      }
    }
  }

  if (strstr(req->content_type, "text/parameters")) {
    if (body != nullptr) {
      const char *vol = strstr((const char *)body, "volume:");
      if (vol != nullptr) {
        float volume = strtof(vol + 7, nullptr);
        rtsp_conn_set_volume(conn, volume);
      }
    }
  } else if (strstr(req->content_type, "application/x-dmap-tagged")) {
    if (body != nullptr && body_len > 0) {
      parse_dmap_metadata(body, body_len, &event_data.metadata, 0);
      has_metadata = true;
    }
  } else if (strstr(req->content_type, "image/jpeg") || strstr(req->content_type, "image/png")) {
    ESP_LOGD(TAG, "Ignoring artwork (%s, %zu bytes): not supported by audio engine yet", req->content_type, body_len);
  } else if (strstr(req->content_type, "application/x-apple-binary-plist")) {
    if (body != nullptr && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      char str_val[TRANSPORT_METADATA_STRING_MAX];
      if (bplist_find_string(body, body_len, "itemName", str_val, sizeof(str_val))) {
        snprintf(event_data.metadata.title, sizeof(event_data.metadata.title), "%s", str_val);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "artistName", str_val, sizeof(str_val))) {
        snprintf(event_data.metadata.artist, sizeof(event_data.metadata.artist), "%s", str_val);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "albumName", str_val, sizeof(str_val))) {
        snprintf(event_data.metadata.album, sizeof(event_data.metadata.album), "%s", str_val);
        has_metadata = true;
      }
      double elapsed = 0, duration = 0;
      if (bplist_find_real(body, body_len, "elapsed", &elapsed)) {
        event_data.metadata.position_secs = (uint32_t)elapsed;
        has_metadata = true;
      }
      if (bplist_find_real(body, body_len, "duration", &duration)) {
        event_data.metadata.duration_secs = (uint32_t)duration;
        has_metadata = true;
      }
    }
  }

  if (has_metadata) {
    transport_events_emit(TRANSPORT_EVENT_METADATA, &event_data);
  }
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_get_parameter(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;
  if (req->body != nullptr && req->body_len > 0 && strstr((const char *)req->body, "volume") != nullptr) {
    char vol_response[32];
    int vol_len = snprintf(vol_response, sizeof(vol_response), "volume: %.2f\r\n", conn->volume_db);
    rtsp_send_response(socket, conn, 200, "OK", req->cseq, "Content-Type: text/parameters\r\n", vol_response,
                       (size_t)vol_len);
    return;
  }
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_pause(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  ESP_LOGI(TAG, "PAUSE received");
  conn->stream_paused = true;
  transport_events_emit(TRANSPORT_EVENT_PAUSED, nullptr);
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_flush(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  // FLUSH — seek + re-preroll the engine. Emit 0 (immediate seek-flush).
  ESP_LOGI(TAG, "FLUSH received");
  TransportEventData data{};
  data.flush.flush_until_ts = 0;
  transport_events_emit(TRANSPORT_EVENT_FLUSH, &data);
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_flushbuffered(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;
  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  uint32_t flush_until_ts = 0;
  if (body != nullptr && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    int64_t from_seq = 0, from_ts = 0, until_seq = 0, until_ts = 0;
    bool got_from_seq = bplist_find_int(body, body_len, "flushFromSeq", &from_seq);
    bool got_from_ts = bplist_find_int(body, body_len, "flushFromTS", &from_ts);
    bool got_until_seq = bplist_find_int(body, body_len, "flushUntilSeq", &until_seq);
    bool got_until_ts = bplist_find_int(body, body_len, "flushUntilTS", &until_ts);
    // A deferred flush keeps playing until the frame at flushUntilTS arrives,
    // then bulk-flushes. Upstream treats it deferred only when all four fields
    // are present; otherwise it is an immediate flush.
    if (got_from_seq && got_from_ts && got_until_seq && got_until_ts) {
      flush_until_ts = (uint32_t) until_ts;
      ESP_LOGI(TAG, "FLUSHBUFFERED deferred: fromSeq=%lld untilTS=%llu", (long long) from_seq,
               (unsigned long long) flush_until_ts);
    } else {
      ESP_LOGI(TAG, "FLUSHBUFFERED immediate");
    }
  } else {
    ESP_LOGI(TAG, "FLUSHBUFFERED immediate/flush");
  }
  TransportEventData data{};
  data.flush.flush_until_ts = flush_until_ts;
  transport_events_emit(TRANSPORT_EVENT_FLUSH, &data);
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_teardown(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  bool has_streams = false;
  size_t stream_count = 0;
  if (body != nullptr && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    if (bplist_get_streams_count(body, body_len, &stream_count)) {
      has_streams = true;
    }
  }
  ESP_LOGI(TAG, "TEARDOWN: has_streams=%d stream_count=%zu", has_streams, stream_count);

  if (has_streams) {
    conn->stream_paused = true;
    transport_events_emit(TRANSPORT_EVENT_PAUSED, nullptr);
  }
  conn->stream_active = false;
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_setrateanchortime(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw,
                                     size_t raw_len) {
  (void)raw;
  (void)raw_len;
  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  double rate = 1.0;
  uint64_t clock_id = 0;
  uint64_t network_time_secs = 0;
  uint64_t network_time_frac = 0;
  uint64_t rtp_time = 0;

  if (body != nullptr && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
    if (!bplist_find_real(body, body_len, "rate", &rate)) {
      int64_t rate_int = 0;
      if (bplist_find_int(body, body_len, "rate", &rate_int)) {
        rate = (double)rate_int;
      }
    }
    int64_t value = 0;
    if (bplist_find_int(body, body_len, "networkTimeTimelineID", &value)) {
      clock_id = (uint64_t)value;
    }
    if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
      network_time_secs = (uint64_t)value;
    }
    if (bplist_find_int(body, body_len, "networkTimeFrac", &value)) {
      network_time_frac = (uint64_t)value;
    }
    if (bplist_find_int(body, body_len, "rtpTime", &value)) {
      rtp_time = (uint64_t)value;
    }
    ESP_LOGI(TAG, "SETRATEANCHORTIME: secs=%llu, rtp=%llu, rate=%.1f", (unsigned long long)network_time_secs,
             (unsigned long long)rtp_time, rate);
    if (network_time_secs != 0 && rtp_time != 0) {
      uint64_t frac = network_time_frac >> 32;
      frac = (frac * 1000000000ULL) >> 32;
      uint64_t network_time_ns = network_time_secs * 1000000000ULL + frac;
      TransportEventData data{};
      data.anchor.clock_id = clock_id;
      data.anchor.network_time_ns = network_time_ns;
      data.anchor.rtp_time = (uint32_t)rtp_time;
      data.anchor.rate = rate;
      transport_events_emit(TRANSPORT_EVENT_ANCHOR, &data);
    }
  }

  if (rate == 0.0) {
    ESP_LOGI(TAG, "SETRATEANCHORTIME: rate=0 -> PAUSING");
    conn->stream_paused = true;
    transport_events_emit(TRANSPORT_EVENT_PAUSED, nullptr);
  } else {
    conn->stream_paused = false;
    transport_events_emit(TRANSPORT_EVENT_PLAYING, nullptr);
  }
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_setpeers(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;
  ESP_LOGI(TAG, "%s: body_len=%zu", req->method, req->body_len);
  rtsp_send_ok(socket, conn, req->cseq);
}

// ===========================================================================
// Dispatch table
// ===========================================================================

typedef void (*rtsp_handler_fn)(int socket, RtspConn *conn, const RtspRequest *req, const uint8_t *raw,
                                size_t raw_len);
typedef struct {
  const char *method;
  rtsp_handler_fn handler;
} RtspMethodHandler;

static const RtspMethodHandler method_handlers[] = {
    {"OPTIONS", handle_options},
    {"GET", handle_get},
    {"POST", handle_post},
    {"ANNOUNCE", handle_announce},
    {"SETUP", handle_setup},
    {"RECORD", handle_record},
    {"SET_PARAMETER", handle_set_parameter},
    {"GET_PARAMETER", handle_get_parameter},
    {"PAUSE", handle_pause},
    {"FLUSH", handle_flush},
    {"FLUSHBUFFERED", handle_flushbuffered},
    {"TEARDOWN", handle_teardown},
    {"SETRATEANCHORTIME", handle_setrateanchortime},
    {"SETPEERS", handle_setpeers},
    {"SETPEERSX", handle_setpeers},
    {nullptr, nullptr},
};

int rtsp_dispatch(int socket, RtspConn *conn, const uint8_t *raw_request, size_t raw_len) {
  RtspRequest req;
  if (rtsp_request_parse(raw_request, raw_len, &req) < 0) {
    ESP_LOGW(TAG, "Failed to parse RTSP request");
    // A malformed request would otherwise hang the client waiting for a reply
    // while holding a speaker/client slot. Reply 400 and let the caller close.
    rtsp_send_response(socket, conn, 400, "Bad Request", 0, "Content-Type: text/plain\r\n", "Bad Request", 11);
    return -1;
  }

  for (const RtspMethodHandler *h = method_handlers; h->method != nullptr; h++) {
    if (strcasecmp(req.method, h->method) == 0) {
      h->handler(socket, conn, &req, raw_request, raw_len);
      return 0;
    }
  }

  ESP_LOGW(TAG, "Unknown method: %s", req.method);
  if (request_uses_rtsp(&req)) {
    rtsp_send_response(socket, conn, 501, "Not Implemented", req.cseq, "Content-Type: text/plain\r\n", "Not Implemented",
                       15);
  } else {
    rtsp_send_http_response(socket, conn, 501, "Not Implemented", "text/plain", "Not Implemented", 15);
  }
  return 0;
}

// ===========================================================================
// RTSP server task
// ===========================================================================

static void signal_old_client_stop(int old_slot) {
  ClientSlot *old = &clients[old_slot];
  if (old->task == nullptr) {
    return;
  }
  ESP_LOGI(TAG, "Signaling old client to stop");
  old->should_stop = true;
  if (old->socket >= 0) {
    shutdown(old->socket, SHUT_RDWR);
  }
}

static void server_task(void *pv) {
  (void)pv;

  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  for (int i = 0; i < 2; i++) {
    clients[i].socket = -1;
    clients[i].conn = nullptr;
    clients[i].task = nullptr;
    clients[i].should_stop = false;
  }

  server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    server_task_handle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  int opt = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(RTSP_PORT);

  if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind RTSP socket: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  if (listen(server_socket, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen on RTSP socket: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);
  server_running = true;

  while (server_running) {
    int new_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
    if (new_socket < 0) {
      if (server_running) {
        ESP_LOGE(TAG, "Failed to accept: %d", errno);
      }
      continue;
    }
    ESP_LOGI(TAG, "New client connected");

    int new_slot = 1 - current_slot;
    if (clients[new_slot].task != nullptr) {
      clients[new_slot].should_stop = true;
      if (clients[new_slot].socket >= 0) {
        shutdown(clients[new_slot].socket, SHUT_RDWR);
      }
      int timeout = 30;
      while (clients[new_slot].task != nullptr && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
      }
      if (clients[new_slot].task != nullptr) {
        ESP_LOGE(TAG, "Slot %d task did not exit in time", new_slot);
        close(new_socket);
        continue;
      }
    }
    signal_old_client_stop(current_slot);

    clients[new_slot].socket = new_socket;
    clients[new_slot].should_stop = false;
    clients[new_slot].task = nullptr;
    BaseType_t ret = xTaskCreate(client_task, "airplay_client", RTSP_CLIENT_STACK_SIZE, (void *)(intptr_t)new_slot, 5,
                                 &clients[new_slot].task);
    if (ret != pdPASS || clients[new_slot].task == nullptr) {
      ESP_LOGE(TAG, "Failed to create client task");
      close(new_socket);
      clients[new_slot].socket = -1;
    } else {
      current_slot = new_slot;
    }
  }

  // Stop all clients.
  for (int i = 0; i < 2; i++) {
    if (clients[i].task != nullptr) {
      clients[i].should_stop = true;
      if (clients[i].socket >= 0) {
        shutdown(clients[i].socket, SHUT_RDWR);
      }
    }
  }
  vTaskDelay(pdMS_TO_TICKS(500));

  if (server_socket >= 0) {
    close(server_socket);
    server_socket = -1;
  }
  server_task_handle = nullptr;
  vTaskDelete(nullptr);
}

// ===========================================================================
// Public API
// ===========================================================================

AirPlay2Transport::AirPlay2Transport() = default;
AirPlay2Transport::~AirPlay2Transport() { this->stop(); }

void AirPlay2Transport::setup(CryptoModule *crypto, const std::string &device_name) {
  if (this->started_) {
    return;
  }
  s_crypto = crypto;
  snprintf(s_device_name, sizeof(s_device_name), "%s", device_name.c_str());

  if (crypto != nullptr) {
    // Ensure the device identity is loaded/generated before advertising pk.
    crypto->device_public_key();
  }

  mdns_airplay_init(device_name.c_str(), crypto != nullptr ? crypto->device_public_key() : nullptr,
                    crypto != nullptr ? 32 : 0);

  BaseType_t ret = xTaskCreate(server_task, "airplay_server", RTSP_SERVER_STACK_SIZE, nullptr, 5, &server_task_handle);
  if (ret != pdPASS || server_task_handle == nullptr) {
    ESP_LOGE(TAG, "Failed to start RTSP server task");
    return;
  }
  this->started_ = true;
  ESP_LOGI(TAG, "AirPlay 2 transport started on port %d", RTSP_PORT);
}

void AirPlay2Transport::loop() {
  // All transport work lives in the RTSP tasks; nothing to poll here yet.
}

void AirPlay2Transport::stop() {
  if (!this->started_) {
    return;
  }
  server_running = false;
  if (server_socket >= 0) {
    shutdown(server_socket, SHUT_RDWR);
    close(server_socket);
    server_socket = -1;
  }
  stop_event_port_task();
  int timeout = 40;
  while (server_task_handle != nullptr && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  this->started_ = false;
}

int AirPlay2Transport::register_event_callback(TransportEventCallback callback, void *user_data) {
  return transport_events_register(callback, user_data);
}

int64_t AirPlay2Transport::current_stream_type() const { return transport_stream_type(); }

}  // namespace airplay_receiver
}  // namespace esphome
