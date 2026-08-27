#pragma once
// airplay_receiver RTSP message parsing + response building (port of
// main/rtsp/rtsp_message.c). All heap in the send helpers routes through
// airplay_* (non-realtime / PSRAM-first).

#include <cstddef>
#include <cstdint>

#include "rtsp_conn.h"

namespace esphome {
namespace airplay_receiver {

#define AIRPLAY_METHOD_MAX 32
#define AIRPLAY_PATH_MAX 256
#define AIRPLAY_PROTOCOL_MAX 16
#define AIRPLAY_CONTENT_TYPE_MAX 64

/**
 * Parsed RTSP request. `body` points into the caller's request buffer (no
 * copy) — it must stay valid for the duration of the handler.
 */
struct RtspRequest {
  char method[AIRPLAY_METHOD_MAX] = {};
  char path[AIRPLAY_PATH_MAX] = {};
  char protocol[AIRPLAY_PROTOCOL_MAX] = {};
  int cseq = 1;
  char content_type[AIRPLAY_CONTENT_TYPE_MAX] = {};
  size_t content_length = 0;
  const uint8_t *body = nullptr;
  size_t body_len = 0;
};

/**
 * Find the end of HTTP/RTSP headers (the CRLFCRLF boundary).
 * @return pointer to the first CR of the terminator, or nullptr.
 */
const uint8_t *rtsp_find_header_end(const uint8_t *data, size_t len);

/// Parse a request. Returns 0 on success, -1 on parse error.
int rtsp_request_parse(const uint8_t *data, size_t len, RtspRequest *req);

/// Find/parse the CSeq header value (defaults to 1).
int rtsp_parse_cseq(const char *request);
/// Find/parse Content-Length (defaults to 0).
int rtsp_parse_content_length(const char *request);
/// Locate the body (returns pointer + length) after the header terminator.
const uint8_t *rtsp_get_body(const char *request, size_t request_len, size_t *body_len);

/// Parse the Transport header's control_port/timing_port (AirPlay 1).
void rtsp_parse_transport(const char *request, uint16_t *control_port, uint16_t *timing_port);

/**
 * Send an RTSP response (encrypts the frame if the connection is in
 * encrypted_mode). `extra_headers` must end with CRLF, or be nullptr.
 */
int rtsp_send_response(int socket, RtspConn *conn, int status_code, const char *status_text, int cseq,
                       const char *extra_headers, const char *body, size_t body_len);

/// Send a simple 200 OK.
int rtsp_send_ok(int socket, RtspConn *conn, int cseq);

/// Send an HTTP response (for GET /info before RTSP mode).
int rtsp_send_http_response(int socket, RtspConn *conn, int status_code, const char *status_text,
                            const char *content_type, const char *body, size_t body_len);

}  // namespace airplay_receiver
}  // namespace esphome
