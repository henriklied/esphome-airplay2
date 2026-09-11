// airplay_receiver buffered TCP audio stream (C++ port of rbouteiller/airplay-esp32
// main/audio/audio_stream_buffered.c, engine-v2 / PR #130).
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
// DECRYPT INTEGRATION (engine-v2): upstream audio_crypto.c was NOT ported.
// Decryption goes through the injected CryptoModule (crypto/crypto_module.h),
// reached via state->crypto (audio_receiver_state_t::crypto), injected by the
// owner (AirPlayReceiver) through audio_receiver_set_crypto_module(). This file
// calls state->crypto->audio_decrypt_buffered(&stream->encrypt, ...) in place
// of the upstream audio_crypto_decrypt_buffered(). The per-stream encryption
// config uses the component's AudioEncrypt (AudioEncryptType::NOT_SET is
// handled inside audio_decrypt_buffered by copying the 12-byte-stripped
// payload).
//
// DECODE INTEGRATION (engine-v2): unlike the realtime path, the buffered path
// does NOT decode on the TCP reader. It demuxes each access unit into an
// audio_encoded_packet_t and hands it to the decode worker
// (audio_decode_worker_enqueue). The worker task calls
// audio_stream_decode_encoded_packet() (audio_stream.cpp) to run the decoder
// and publish PCM onto the engine-v2 timeline. Decoding off the reader keeps
// the socket draining through decode hiccups instead of dropping packets.

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
// Bounded hand-off to the decode worker.  Long enough to ride out a decode
// hiccup, short enough that a wedged decoder cannot block stream teardown.
#define BUFFERED_ENQUEUE_TIMEOUT_MS 200U

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

// Read exact number of bytes, but keep waiting on timeout if paused
// Returns: positive = bytes read, 0 = connection closed, -1 = error
static ssize_t read_exact(audio_stream_t *stream, audio_receiver_state_t *state,
                          int sock, uint8_t *buf, size_t len) {
  size_t total = 0;
  while (total < len && stream->running) {
    ssize_t n = recv(sock, buf + total, len - total, 0);
    if (n > 0) {
      total += (size_t) n;
    } else if (n == 0) {
      // Connection closed by peer
      ESP_LOGI(TAG, "Buffered audio connection closed by peer");
      return 0;
    } else {
      // n < 0: error or timeout
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Timeout - if we're paused, keep waiting for resume
        if (!state->timing.playing) {
          // Still paused, keep the connection alive
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        // Playing but timed out - connection may be dead
        ESP_LOGW(TAG, "Buffered audio timeout while playing");
        return -1;
      }
      ESP_LOGE(TAG, "Buffered audio recv error: %d", errno);
      return -1;
    }
  }
  return stream->running ? (ssize_t) total : -1;
}

static void buffered_audio_task(void *pvParameters) {
  audio_stream_t *stream = (audio_stream_t *) pvParameters;
  audio_receiver_state_t *state = audio_stream_state(stream);

  while (stream->running) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_sock = accept(state->buffered_listen_socket,
                             (struct sockaddr *) &client_addr, &addr_len);
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
    // buffer can hold exactly what the TCP window allows in flight.  A larger
    // SO_RCVBUF (e.g. the old 65536) accumulates stale audio data that must
    // drain through the RTP gates on every track skip, adding transition
    // latency.  Keeping it at TCP_WND ties both knobs to a single sdkconfig
    // value (CONFIG_LWIP_TCP_WND_DEFAULT).
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
      // Back-pressure: if the pipeline is nearly full, pause reading to let
      // TCP flow control slow down the sender. This prevents overflow and
      // keeps frames in order.
      while (stream->running &&
             (audio_decode_worker_is_nearly_full(state->decode_worker) ||
              audio_engine_v2_is_nearly_full(&state->engine_v2))) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }

      uint8_t len_buf[2];
      if (read_exact(stream, state, client_sock, len_buf, 2) != 2) {
        break;
      }

      uint16_t data_len = (uint16_t) ((len_buf[0] << 8) | len_buf[1]);
      // The 2-byte length prefix INCLUDES itself; a real frame has at least a
      // 12-byte RTP header, so require data_len >= 14 (packet_len >= 12).
      // Otherwise packet[1..7] would be read from an unread (stale) buffer.
      if (data_len < 14 || data_len > BUFFERED_AUDIO_PACKET_SIZE) {
        ESP_LOGW(TAG, "Invalid buffered audio packet length: %u", data_len);
        break;
      }

      size_t packet_len = (size_t) (data_len - 2);
      if (read_exact(stream, state, client_sock, packet, packet_len) !=
          (ssize_t) packet_len) {
        break;
      }

      state->stats.packets_received++;

      uint32_t seq_no = ((uint32_t) packet[1] << 16) | ((uint32_t) packet[2] << 8) | packet[3];
      uint32_t timestamp = ((uint32_t) packet[4] << 24) | ((uint32_t) packet[5] << 16) |
                           ((uint32_t) packet[6] << 8) | packet[7];

      // Snapshot the epoch before the gate so a seek that lands between the
      // gate and the decode invalidates this packet rather than letting it
      // reach the timeline of the new segment.
      const uint32_t epoch = audio_epoch_get(&state->engine_v2.epoch);
      (void) __atomic_add_fetch(&state->engine_v2.diag_rx_packets, 1U,
                                __ATOMIC_RELAXED);

      // Drop stale pre-seek/old-track packets before AES and AAC work.  The
      // bytes still have to be drained from TCP (done above), but they no
      // longer consume decoder time or enter the PCM ring buffer.
      if (!audio_stream_accept_timestamp(state, timestamp)) {
        state->stats.packets_dropped++;
        (void) __atomic_add_fetch(&state->engine_v2.diag_gate_drops, 1U,
                                  __ATOMIC_RELAXED);
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

      int decrypted_len = state->crypto->audio_decrypt_buffered(
          &stream->encrypt, packet, packet_len, decrypted, decrypt_capacity);
      if (decrypted_len < 0) {
        state->stats.decrypt_errors++;
        state->stats.packets_dropped++;
        continue;
      }

      state->stats.last_seq = (uint16_t) (seq_no & 0xFFFF);
      state->stats.last_timestamp = timestamp;

      state->blocks_read++;
      state->blocks_read_in_sequence++;

      // Hand the access unit to the decode worker.  Decoding on this task
      // would stall the TCP reader for the duration of every AAC frame, which
      // is what previously turned a transient decode hiccup into dropped
      // packets and a visible gap.
      const audio_encoded_packet_t encoded = {
          .epoch = epoch,
          .rtp_timestamp = timestamp,
          .payload = decrypted,
          .payload_len = (size_t) decrypted_len,
          .prime_mute = audio_stream_aac_prime_mute_wanted(state),
      };

      const audio_decode_enqueue_result_t enq =
          audio_decode_worker_enqueue(state->decode_worker, &encoded,
                                      BUFFERED_ENQUEUE_TIMEOUT_MS);
      if (enq == AUDIO_DECODE_ENQUEUE_OK) {
        (void) __atomic_add_fetch(&state->engine_v2.diag_enqueue_ok, 1U,
                                  __ATOMIC_RELAXED);
      } else {
        state->stats.packets_dropped++;
        (void) __atomic_add_fetch(
            enq == AUDIO_DECODE_ENQUEUE_RETRY
                ? &state->engine_v2.diag_enqueue_retries
                : &state->engine_v2.diag_queue_drops,
            1U, __ATOMIC_RELAXED);
      }
    }

    close(client_sock);
    state->buffered_client_socket = -1;
  }

  state->buffered_task_handle = nullptr;
  vTaskDelete(nullptr);
}

static bool buffered_wait_for_task_stopped(audio_receiver_state_t *state,
                                           int timeout_ticks) {
  while (state->buffered_task_handle && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return state->buffered_task_handle == nullptr;
}

static esp_err_t buffered_start(audio_stream_t *stream, uint16_t port) {
  audio_receiver_state_t *state = audio_stream_state(stream);
  if (stream->running) {
    if (state->buffered_port == port) {
      ESP_LOGI(TAG, "Buffered audio already running on port %u, continuing",
               (unsigned) port);
      return ESP_OK;
    }
    // Serving a port we are not bound to is the silent-takeover bug: the
    // sender connects to the advertised port, nothing answers, and the stream
    // wedges with no audio and no error.  The caller stops the stream before
    // rebinding; refuse rather than report success on the wrong port.
    ESP_LOGE(TAG, "Buffered audio bound to %u, asked to serve %u",
             (unsigned) state->buffered_port, (unsigned) port);
    return ESP_ERR_INVALID_STATE;
  }
  if (state->buffered_task_handle) {
    ESP_LOGW(TAG, "Buffered audio task still stopping, waiting");
    if (!buffered_wait_for_task_stopped(state, 20)) {
      ESP_LOGW(TAG, "Buffered audio task still active");
      return ESP_ERR_INVALID_STATE;
    }
  }

  uint16_t bound_port = port;
  state->buffered_listen_socket =
      socket_utils_bind_tcp_listener(port, 1, true, &bound_port);
  if (state->buffered_listen_socket < 0) {
    return ESP_FAIL;
  }
  state->buffered_port = bound_port;

  stream->running = true;

  state->buffered_task_handle = nullptr;
  BaseType_t task_ret =
      xTaskCreate(buffered_audio_task, "buff_audio", AUDIO_BUFFERED_STACK_SIZE,
                  stream, 5, &state->buffered_task_handle);
  if (task_ret != pdPASS || !state->buffered_task_handle) {
    ESP_LOGE(TAG, "Failed to create buffered audio task");
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
    stream->running = false;
    return ESP_FAIL;
  }

  // Worst-case memory snapshot: buffered streaming is the heaviest concurrent
  // load (WiFi + lwip + decoder + PCM ring).  Use this to size WiFi/TCP buffers
  // without risking OOM.  Reports go through the airplay_* allocator so they
  // observe the same memory policy the rest of the component uses.
  ESP_LOGI(TAG,
           "Buffered start: free heap %lu internal (largest block %lu), "
           "%lu SPIRAM",
           (unsigned long) airplay_internal_free(),
           (unsigned long) airplay_internal_largest_block(),
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

static bool buffered_is_running(audio_stream_t *stream) {
  return stream->running;
}

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

// The base audio_stream slice declares this ops object externally and binds it
// when it builds the buffered stream via audio_stream_create_buffered().  The
// vtable field order matches upstream audio_stream.h 1:1.
// `extern` guarantees external linkage (a plain const namespace-scope object
// would otherwise get internal linkage per C++ [basic.link]).
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
