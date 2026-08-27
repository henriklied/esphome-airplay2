// airplay_receiver RTSP message parsing + response building (port of main/rtsp/rtsp_message.c).
#include "rtsp_message.h"

#include <sys/socket.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>

#include "../allocator.h"
#include "esphome/core/log.h"
#include "rtsp_crypto.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_rtsp_msg";

// Case-insensitive substring search (stratospherically safer than relying on
// GNU strcasestr; the port keeps the same semantics).
static const char *ci_strstr(const char *haystack, const char *needle) {
  if (haystack == nullptr || needle == nullptr) {
    return nullptr;
  }
  size_t nlen = strlen(needle);
  if (nlen == 0) {
    return haystack;
  }
  for (const char *p = haystack; *p != '\0'; ++p) {
    size_t i = 0;
    for (; i < nlen; ++i) {
      char a = p[i];
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
      return p;
    }
  }
  return nullptr;
}

const uint8_t *rtsp_find_header_end(const uint8_t *data, size_t len) {
  for (size_t i = 0; i + 3 < len; i++) {
    if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
      return data + i;
    }
  }
  return nullptr;
}

int rtsp_parse_cseq(const char *request) {
  const char *cseq = strstr(request, "CSeq:");
  if (cseq == nullptr) {
    cseq = ci_strstr(request, "cseq:");
  }
  if (cseq != nullptr) {
    return (int)strtol(cseq + 5, nullptr, 10);
  }
  return 1;
}

int rtsp_parse_content_length(const char *request) {
  const char *cl = strstr(request, "Content-Length:");
  if (cl == nullptr) {
    cl = ci_strstr(request, "content-length:");
  }
  if (cl != nullptr) {
    return (int)strtol(cl + 15, nullptr, 10);
  }
  return 0;
}

const uint8_t *rtsp_get_body(const char *request, size_t request_len, size_t *body_len) {
  const char *body = strstr(request, "\r\n\r\n");
  if (body != nullptr) {
    body += 4;
    *body_len = request_len - (size_t)(body - request);
    return (const uint8_t *)body;
  }
  *body_len = 0;
  return nullptr;
}

void rtsp_parse_transport(const char *request, uint16_t *control_port, uint16_t *timing_port) {
  if (control_port != nullptr) {
    *control_port = 0;
  }
  if (timing_port != nullptr) {
    *timing_port = 0;
  }

  const char *transport = ci_strstr(request, "Transport:");
  if (transport == nullptr) {
    return;
  }
  // Limit search to the Transport header line.
  const char *line_end = strstr(transport, "\r\n");
  if (line_end == nullptr) {
    line_end = transport + strlen(transport);
  }

  const char *cp = ci_strstr(transport, "control_port=");
  if (cp != nullptr && cp < line_end && control_port != nullptr) {
    *control_port = (uint16_t)strtoul(cp + 13, nullptr, 10);
  }
  const char *tp = ci_strstr(transport, "timing_port=");
  if (tp != nullptr && tp < line_end && timing_port != nullptr) {
    *timing_port = (uint16_t)strtoul(tp + 12, nullptr, 10);
  }
}

int rtsp_request_parse(const uint8_t *data, size_t len, RtspRequest *req) {
  if (data == nullptr || req == nullptr || len == 0) {
    return -1;
  }

  memset(req, 0, sizeof(*req));
  req->cseq = 1;

  const uint8_t *header_end = rtsp_find_header_end(data, len);
  if (header_end == nullptr) {
    return -1;
  }

  const uint8_t *line_end = (const uint8_t *)memchr(data, '\n', (size_t)(header_end - data));
  if (line_end == nullptr) {
    return -1;
  }
  size_t line_len = (size_t)(line_end - data);
  if (line_len > 0 && data[line_len - 1] == '\r') {
    line_len--;
  }
  char first_line[320];
  if (line_len >= sizeof(first_line)) {
    return -1;
  }
  memcpy(first_line, data, line_len);
  first_line[line_len] = '\0';
  if (sscanf(first_line, "%31s %255s %15s", req->method, req->path, req->protocol) < 2) {
    return -1;
  }

  req->cseq = rtsp_parse_cseq((const char *)data);
  int cl = rtsp_parse_content_length((const char *)data);
  req->content_length = cl > 0 ? (size_t) cl : 0;

  const char *ct = strstr((const char *)data, "Content-Type:");
  if (ct == nullptr) {
    ct = ci_strstr((const char *)data, "content-type:");
  }
  if (ct != nullptr) {
    sscanf(ct, "Content-Type: %63s", req->content_type);
  }

  req->body = rtsp_get_body((const char *)data, len, &req->body_len);
  return 0;
}

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

// Build the RTSP header block and dispatch to either the encrypted frame
// writer or a plain send. Shared by rtsp_send_response and rtsp_send_ok.
static int send_rtsp(int socket, RtspConn *conn, int status_code, const char *status_text, int cseq,
                     const char *extra_headers, const char *body, size_t body_len) {
  char header[1024];
  int header_len;

  if (extra_headers != nullptr && body != nullptr && body_len > 0) {
    header_len = snprintf(header, sizeof(header),
                          "RTSP/1.0 %d %s\r\n"
                          "CSeq: %d\r\n"
                          "Server: AirTunes/377.40.00\r\n"
                          "%s"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          status_code, status_text, cseq, extra_headers, body_len);
  } else if (extra_headers != nullptr) {
    header_len = snprintf(header, sizeof(header),
                          "RTSP/1.0 %d %s\r\n"
                          "CSeq: %d\r\n"
                          "Server: AirTunes/377.40.00\r\n"
                          "%s"
                          "\r\n",
                          status_code, status_text, cseq, extra_headers);
  } else if (body != nullptr && body_len > 0) {
    header_len = snprintf(header, sizeof(header),
                          "RTSP/1.0 %d %s\r\n"
                          "CSeq: %d\r\n"
                          "Server: AirTunes/377.40.00\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          status_code, status_text, cseq, body_len);
  } else {
    header_len = snprintf(header, sizeof(header),
                          "RTSP/1.0 %d %s\r\n"
                          "CSeq: %d\r\n"
                          "Server: AirTunes/377.40.00\r\n"
                          "\r\n",
                          status_code, status_text, cseq);
  }

  size_t total_len = (size_t)header_len + body_len;
  uint8_t *response = static_cast<uint8_t *>(airplay_alloc(total_len, false));
  if (response == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate response buffer (%zu bytes)", total_len);
    return -1;
  }

  memcpy(response, header, (size_t)header_len);
  if (body != nullptr && body_len > 0) {
    memcpy(response + header_len, body, body_len);
  }

  int result;
  if (conn != nullptr && conn->encrypted_mode) {
    result = rtsp_crypto_write_frame(socket, conn, response, total_len);
  } else {
    result = (send_all(socket, response, total_len) < 0) ? -1 : 0;
    if (result < 0) {
      ESP_LOGE(TAG, "Failed to send RTSP response");
    }
  }

  airplay_free(response);
  return result;
}

int rtsp_send_response(int socket, RtspConn *conn, int status_code, const char *status_text, int cseq,
                       const char *extra_headers, const char *body, size_t body_len) {
  return send_rtsp(socket, conn, status_code, status_text, cseq, extra_headers, body, body_len);
}

int rtsp_send_ok(int socket, RtspConn *conn, int cseq) {
  return send_rtsp(socket, conn, 200, "OK", cseq, nullptr, nullptr, 0);
}

int rtsp_send_http_response(int socket, RtspConn *conn, int status_code, const char *status_text,
                            const char *content_type, const char *body, size_t body_len) {
  char header[512];
  int header_len = snprintf(header, sizeof(header),
                            "HTTP/1.1 %d %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Server: AirTunes/377.40.00\r\n"
                            "CSeq: 1\r\n"
                            "\r\n",
                            status_code, status_text, content_type, body_len);

  size_t total_len = (size_t)header_len + body_len;
  uint8_t *response = static_cast<uint8_t *>(airplay_alloc(total_len, false));
  if (response == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate HTTP response buffer");
    return -1;
  }

  memcpy(response, header, (size_t)header_len);
  if (body != nullptr && body_len > 0) {
    memcpy(response + header_len, body, body_len);
  }

  int result;
  if (conn != nullptr && conn->encrypted_mode) {
    result = rtsp_crypto_write_frame(socket, conn, response, total_len);
  } else {
    result = (send_all(socket, response, total_len) < 0) ? -1 : 0;
  }

  airplay_free(response);
  return result;
}

}  // namespace airplay_receiver
}  // namespace esphome
