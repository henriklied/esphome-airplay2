// airplay_receiver buffered TCP audio stream (C++ port of rbouteiller/airplay-esp32
// main/audio/audio_stream_buffered.c).
//
// AirPlay 2 stream type 103 is a *buffered* stream: the source pushes a
// length-prefixed, ChaCha20-Poly1305-encrypted ALAC/AAC packet stream over a
// single long-lived TCP connection that this receiver owns (vs. the realtime
// type 96 path, which is RTP-over-UDP).  This file implements the receiving
// half of that path as a FreeRTOS task bound to a TCP listener.
//
// SHARED CONTRACT (see audio_receiver.h / audio_stream.h):
//   * namespace esphome::airplay_receiver;
//   * ALL heap routed through ../allocator.h (airplay_alloc / airplay_free) —
//     no malloc/calloc/heap_caps_* directly;
//   * diagnostics via ESP_LOGx (esphome/core/log.h) with a file-local TAG;
//   * upstream signatures preserved 1:1 (function names, parameter lists and
//     the audio_stream_ops_t vtable layout are unchanged).
//
// DECRYPT INTEGRATION: upstream audio_crypto.c was NOT ported.  Decryption now
// goes through the injected CryptoModule (crypto/crypto_module.h).  The stream
// reaches it via state->crypto (audio_receiver_state_t::crypto), injected by
// the owner (AirPlayReceiver) through audio_receiver_set_crypto_module().  This
// file calls state->crypto->audio_decrypt_buffered(&stream->encrypt, ...) in
// place of the upstream audio_crypto_decrypt_buffered(), exactly as
// audio/audio_stream.h documents.  The per-stream encryption config uses the
// component's AudioEncrypt (AudioEncryptType::NOT_SET == plaintext, handled
// inside audio_decrypt_buffered by copying the payload).
//
// This file DEFINES the buffered stream's operations (the *_ops vtable) but
// does NOT define the base audio_stream API.  The surrounding types and the
// shared pipeline helpers (audio_stream_t / audio_stream_ops_t /
// audio_stream_state() / audio_receiver_state_t / audio_stream_accept_timestamp()
// / audio_stream_process_accepted_frame() / audio_buffer_is_nearly_full()) are
// supplied by the sibling slices:
//   * audio/audio_stream.h -> base stream descriptor + ops vtable
//     (audio_stream_t / audio_stream_ops_t);
//   * audio/audio_receiver_internal.h -> receiver state, audio_stream_state(),
//     the RTP gate / decode-and-queue helpers (declarations; the logic lives in
//     the base audio_stream.cpp).  Included AFTER audio_stream.h, as upstream
//     does, so the full audio_stream_t is visible before the state struct.
//   * audio/audio_buffer.h -> PCM ring (audio_buffer_is_nearly_full for
//     back-pressure);
//   * crypto/crypto_module.h -> CryptoModule::audio_decrypt_buffered /
//     AudioEncrypt.

#include "audio_stream.h"
#include "audio_receiver_internal.h"

#include "../allocator.h"
#include "../transport/socket_utils.h"

#include "esphome/core/log.h"

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstddef>

// Task stack size: prefer the platform profile (platform/<variant>/config.h)
// when a native build puts it on the include path, else the upstream value that
// is inlined there.  The ESPHome external-component build does not copy the
// two-level platform headers, so the fallback below is what actually builds.
#ifndef AIRPLAY_TASK_STACK_AUDIO_BUFFERED
#define AIRPLAY_TASK_STACK_AUDIO_BUFFERED 4096
#endif

#define BUFFERED_AUDIO_PACKET_SIZE 8192
#define AUDIO_BUFFERED_STACK_SIZE AIRPLAY_TASK_STACK_AUDIO_BUFFERED

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "audio_buf";

// Tie the socket receive buffer to the TCP window (upstream comment): a larger
// SO_RCVBUF would accumulate stale audio that has to drain through the RTP
// gates on every track skip, adding transition latency.  When the ESP-IDF
// sdkconfig macro is absent (e.g. this file is compile-checked standalone) fall
// back to a portable default.
#ifndef CONFIG_LWIP_TCP_WND_DEFAULT
#define CONFIG_LWIP_TCP_WND_DEFAULT 32768
#endif

// Read exactly `len` bytes, but keep waiting on a socket timeout while the
// stream is paused (the connection stays alive across a pause/resume).
// Returns: positive = bytes read, 0 = connection closed, -1 = error.
static ssize_t read_exact(audio_stream_t *stream, audio_receiver_state_t *state, int sock,
                          uint8_t *buf, size_t len) {
  size_t total = 0;
  while (total < len && stream->running) {
    ssize_t n = recv(sock, buf + total, len - total, 0);
    if (n > 0) {
      total += (size_t) n;
    } else if (n == 0) {
      // Connection closed by peer.
      ESP_LOGI(TAG, "Buffered audio connection closed by peer");
      return 0;
    } else {
      // n < 0: error or timeout.
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Timeout - if we're paused, keep waiting for resume.
        if (!state->timing.playing) {
          // Still paused: keep the connection alive.
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        // Playing but timed out - connection may be dead.
        ESP_LOGW(TAG, "Buffered audio timeout while playing");
        return -1;
      }
      ESP_LOGE(TAG, "Buffered audio recv error: %d", errno);
      return -1;
    }
  }
  return stream->running ? (ssize_t) total : -1;
}

static void buffered_audio_task(void *pv_parameters) {
  audio_stream_t *stream = (audio_stream_t *) pv_parameters;
  audio_receiver_state_t *state = audio_stream_state(stream);

  while (stream->running) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_sock = accept(state->buffered_listen_socket, (struct sockaddr *) &client_addr, &addr_len);
    if (client_sock < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && stream->running) {
        ESP_LOGE(TAG, "Buffered audio accept error: %d", errno);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    state->buffered_client_socket = client_sock;

    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Socket receive buffer: match lwIP's TCP receive window so the kernel
    // buffer holds exactly what the TCP window allows in flight (see upstream
    // comment; CONFIG_LWIP_TCP_WND_DEFAULT ties it to the sdkconfig knob).
    int rcvbuf = CONFIG_LWIP_TCP_WND_DEFAULT;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    uint8_t *packet = state->buffered_recv_buffer;
    if (!packet) {
      // Socket payload is non-realtime: PSRAM-first, internal fallback for
      // small requests (the default airplay_alloc(false) policy).
      packet = (uint8_t *) airplay_alloc(BUFFERED_AUDIO_PACKET_SIZE, false);
      if (!packet) {
        ESP_LOGE(TAG, "Failed to allocate buffered audio packet buffer");
        close(client_sock);
        state->buffered_client_socket = -1;
        continue;
      }
      state->buffered_recv_buffer = packet;
    }

    while (stream->running) {
      // Back-pressure: if the PCM ring is nearly full, pause reading so TCP
      // flow control slows the sender and frames stay in order.
      while (audio_buffer_is_nearly_full(&state->buffer) && stream->running) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }

      uint8_t len_buf[2];
      if (read_exact(stream, state, client_sock, len_buf, 2) != 2) {
        break;
      }

      uint16_t data_len = (uint16_t) ((len_buf[0] << 8) | len_buf[1]);
      if (data_len < 2 || data_len > BUFFERED_AUDIO_PACKET_SIZE) {
        ESP_LOGW(TAG, "Invalid buffered audio packet length: %u", data_len);
        break;
      }

      size_t packet_len = (size_t) (data_len - 2);
      if (read_exact(stream, state, client_sock, packet, packet_len) != (ssize_t) packet_len) {
        break;
      }

      state->stats.packets_received++;

      uint32_t seq_no = ((uint32_t) packet[1] << 16) | ((uint32_t) packet[2] << 8) | packet[3];
      uint32_t timestamp = ((uint32_t) packet[4] << 24) | ((uint32_t) packet[5] << 16) |
                           ((uint32_t) packet[6] << 8) | packet[7];

      // Drop stale pre-seek/old-track packets before decrypt and decode.  The
      // bytes still have to be drained from TCP (done above), but they no
      // longer consume decode time or enter the PCM ring.
      if (!audio_stream_accept_timestamp(state, timestamp)) {
        state->stats.packets_dropped++;
        continue;
      }

      uint8_t *decrypted = state->decrypt_buffer;
      size_t decrypt_capacity = state->decrypt_buffer_size;
      if (!decrypted) {
        decrypted = packet + 12;
        decrypt_capacity = packet_len > 12 ? packet_len - 12 : 0;
      }

      if (!state->crypto) {
        ESP_LOGE(TAG, "Buffered decrypt requested but no CryptoModule injected");
        state->stats.decrypt_errors++;
        state->stats.packets_dropped++;
        continue;
      }

      int decrypted_len =
          state->crypto->audio_decrypt_buffered(&stream->encrypt, packet, packet_len, decrypted, decrypt_capacity);
      if (decrypted_len < 0) {
        state->stats.decrypt_errors++;
        state->stats.packets_dropped++;
        continue;
      }

      state->stats.last_seq = (uint16_t) (seq_no & 0xFFFFu);
      state->stats.last_timestamp = timestamp;

      state->blocks_read++;
      state->blocks_read_in_sequence++;

      if (!audio_stream_process_accepted_frame(state, timestamp, decrypted, (size_t) decrypted_len)) {
        state->stats.packets_dropped++;
      }
    }

    close(client_sock);
    state->buffered_client_socket = -1;
  }

  state->buffered_task_handle = nullptr;
  vTaskDelete(nullptr);
}

static bool buffered_wait_for_task_stopped(audio_receiver_state_t *state, int timeout_ticks) {
  while (state->buffered_task_handle && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return state->buffered_task_handle == nullptr;
}

static esp_err_t buffered_start(audio_stream_t *stream, uint16_t port) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (stream->running) {
    ESP_LOGI(TAG, "Buffered audio already running, continuing");
    return ESP_OK;
  }
  if (state->buffered_task_handle) {
    ESP_LOGW(TAG, "Buffered audio task still stopping, waiting");
    if (!buffered_wait_for_task_stopped(state, 20)) {
      ESP_LOGW(TAG, "Buffered audio task still active");
      return ESP_ERR_INVALID_STATE;
    }
  }

  uint16_t bound_port = port;
  state->buffered_listen_socket = socket_utils_bind_tcp_listener(port, 1, true, &bound_port);
  if (state->buffered_listen_socket < 0) {
    return ESP_FAIL;
  }
  state->buffered_port = bound_port;

  stream->running = true;

  state->buffered_task_handle = nullptr;
  BaseType_t task_ret = xTaskCreate(buffered_audio_task, "buff_audio", AUDIO_BUFFERED_STACK_SIZE, stream, 5,
                                    &state->buffered_task_handle);
  if (task_ret != pdPASS || !state->buffered_task_handle) {
    ESP_LOGE(TAG, "Failed to create buffered audio task");
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
    stream->running = false;
    return ESP_FAIL;
  }

  // Worst-case memory snapshot: buffered streaming is the heaviest concurrent
  // load (WiFi + lwip + decoder + PCM ring).  Reports go through the airplay_*
  // allocator so they observe the same memory policy the rest of the component
  // uses.
  ESP_LOGI(TAG, "Buffered start: free heap %lu internal (largest block %lu), %lu SPIRAM",
           (unsigned long) airplay_internal_free(), (unsigned long) airplay_internal_largest_block(),
           (unsigned long) airplay_psram_free());

  return ESP_OK;
}

static void buffered_stop(audio_stream_t *stream) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (!stream->running && !state->buffered_task_handle) {
    return;
  }

  stream->running = false;

  if (state->buffered_client_socket > 0) {
    close(state->buffered_client_socket);
    state->buffered_client_socket = -1;
  }

  if (state->buffered_listen_socket > 0) {
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
  }

  if (!buffered_wait_for_task_stopped(state, 20)) {
    ESP_LOGW(TAG, "Buffered audio task did not exit within timeout");
    return;
  }

  if (state->buffered_recv_buffer) {
    airplay_free(state->buffered_recv_buffer);
    state->buffered_recv_buffer = nullptr;
  }

  state->buffered_port = 0;
}

static uint16_t buffered_get_port(audio_stream_t *stream) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  return state->buffered_port;
}

static bool buffered_is_running(audio_stream_t *stream) { return stream->running; }

static void buffered_destroy(audio_stream_t *stream) {
  if (!stream) {
    return;
  }

  buffered_stop(stream);
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (state->buffered_task_handle) {
    ESP_LOGW(TAG, "Leaking buffered stream because task shutdown timed out");
    return;
  }
  airplay_free(stream);
}

// The base audio_stream slice declares this ops object externally
// (`extern const audio_stream_ops_t audio_stream_buffered_ops;`) and binds it
// when it builds the buffered stream via audio_stream_create_buffered().  The
// vtable field order matches upstream audio_stream.h 1:1.
//
// `extern` is intentional: per C++ [basic.link] a const namespace-scope object
// has INTERNAL linkage unless explicitly declared extern, so declaring it
// extern guarantees external linkage and matches the base slice's extern
// declaration portably across compilers (a plain `const ... x = {..}` is
// compiler-dependent here).
extern const audio_stream_ops_t audio_stream_buffered_ops = {
    .start = buffered_start,        // start
    .stop = buffered_stop,          // stop
    .receive_packet = nullptr,      // receive_packet (TCP path reads inline)
    .decrypt_payload = nullptr,     // decrypt_payload (decrypted in run loop)
    .get_port = buffered_get_port,  // get_port
    .is_running = buffered_is_running,  // is_running
    .destroy = buffered_destroy,        // destroy
};

}  // namespace airplay_receiver
}  // namespace esphome
