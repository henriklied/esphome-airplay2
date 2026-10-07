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
//
// RING (buffered_ring.h): the socket drains into a PSRAM byte ring sized to
// the advertised audioBufferSize, and frames leave the ring only while the
// pipeline has room. Reading the socket only when the pipeline had room kept
// the TCP window shut and cost ~1 s of silence per sender persist-timer stall.

#include "audio_stream.h"
#include "buffered_ring.h"
#include "audio_receiver_internal.h"

#include "../allocator.h"
#include "../transport/socket_utils.h"

#include "esphome/core/log.h"

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
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

// Tie the socket receive buffer to the TCP window. Deep staging is the ring's
// job; stale audio it holds after a skip drains through the RTP gates before
// decrypt, so the cost is a memcpy per frame. When the ESP-IDF sdkconfig macro
// is absent (e.g. this file is compile-checked standalone) fall back to a
// portable default.
#ifndef CONFIG_LWIP_TCP_WND_DEFAULT
#define CONFIG_LWIP_TCP_WND_DEFAULT 32768
#endif

// Prefer the full advertised size; halve on allocation failure down to this.
// Below it the window closes about as often as it did without the ring.
#define BUFFERED_RING_MIN_BYTES (64U * 1024U)
// Socket wait per pass when nothing moved; bounds how late a freed timeline
// slot is refilled.
#define BUFFERED_POLL_MS 10
// Frames fed per pass, so draining a stale backlog still services the socket.
#define BUFFERED_FRAMES_PER_PASS 32U
// A playing stream whose window is open but delivers nothing this long is dead.
#define BUFFERED_IDLE_TIMEOUT_MS 30000U

static bool buffered_ensure_ring(audio_receiver_state_t *state) {
  if (state->buffered_ring_storage) {
    return true;
  }
  for (size_t capacity = BUFFERED_AUDIO_BUFFER_BYTES; capacity >= BUFFERED_RING_MIN_BYTES;
       capacity /= 2U) {
    auto *storage = (uint8_t *) airplay_alloc(capacity, false);
    if (!storage) {
      continue;
    }
    if (capacity < BUFFERED_AUDIO_BUFFER_BYTES) {
      ESP_LOGW(TAG, "Buffered ring is %u KB, below the %u KB advertised: expect window stalls",
               (unsigned) (capacity / 1024U), (unsigned) (BUFFERED_AUDIO_BUFFER_BYTES / 1024U));
    }
    state->buffered_ring_storage = storage;
    state->buffered_ring_capacity = capacity;
    return true;
  }
  return false;
}

static bool buffered_pipeline_has_room(audio_receiver_state_t *state) {
  return !audio_decode_worker_is_nearly_full(state->decode_worker) &&
         !audio_engine_v2_is_nearly_full(&state->engine_v2);
}

// Gate, decrypt and hand one frame (length prefix already stripped) to the
// decode worker.
static void buffered_process_frame(audio_stream_t *stream, audio_receiver_state_t *state,
                                   uint8_t *packet, size_t packet_len) {
  state->stats.packets_received++;

  uint32_t seq_no = ((uint32_t) packet[1] << 16) | ((uint32_t) packet[2] << 8) | packet[3];
  uint32_t timestamp = ((uint32_t) packet[4] << 24) | ((uint32_t) packet[5] << 16) |
                       ((uint32_t) packet[6] << 8) | packet[7];
  uint32_t ssrc = ((uint32_t) packet[8] << 24) | ((uint32_t) packet[9] << 16) |
                  ((uint32_t) packet[10] << 8) | packet[11];

  // Snapshot the epoch before the gate so a seek that lands between the
  // gate and the decode invalidates this packet rather than letting it
  // reach the timeline of the new segment.
  const uint32_t epoch = audio_epoch_get(&state->engine_v2.epoch);
  (void) __atomic_add_fetch(&state->engine_v2.diag_rx_packets, 1U, __ATOMIC_RELAXED);

  // Drop stale pre-seek/old-track packets before AES and AAC work.  The
  // bytes still have to be drained from the ring, but they no longer
  // consume decoder time or enter the PCM ring buffer.
  if (!audio_stream_accept_timestamp(state, timestamp)) {
    state->stats.packets_dropped++;
    (void) __atomic_add_fetch(&state->engine_v2.diag_gate_drops, 1U, __ATOMIC_RELAXED);
    return;
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
    return;
  }

  int decrypted_len = state->crypto->audio_decrypt_buffered(
      &stream->encrypt, packet, packet_len, decrypted, decrypt_capacity);
  if (decrypted_len < 0) {
    state->stats.decrypt_errors++;
    state->stats.packets_dropped++;
    return;
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
      .ssrc = ssrc,
      .payload = decrypted,
      .payload_len = (size_t) decrypted_len,
      .prime_mute = audio_stream_aac_prime_mute_wanted(state),
  };

  const audio_decode_enqueue_result_t enq =
      audio_decode_worker_enqueue(state->decode_worker, &encoded, BUFFERED_ENQUEUE_TIMEOUT_MS);
  if (enq == AUDIO_DECODE_ENQUEUE_OK) {
    (void) __atomic_add_fetch(&state->engine_v2.diag_enqueue_ok, 1U, __ATOMIC_RELAXED);
  } else {
    state->stats.packets_dropped++;
    (void) __atomic_add_fetch(enq == AUDIO_DECODE_ENQUEUE_RETRY
                                  ? &state->engine_v2.diag_enqueue_retries
                                  : &state->engine_v2.diag_queue_drops,
                              1U, __ATOMIC_RELAXED);
  }
}

static void buffered_wait_readable(int sock) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(sock, &readable);
  struct timeval tv = {.tv_sec = 0, .tv_usec = BUFFERED_POLL_MS * 1000};
  (void) select(sock + 1, &readable, nullptr, nullptr, &tv);
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
    const unsigned peer_port = ntohs(client_addr.sin_port);
    uint32_t connection_packets = 0;
    ESP_LOGI(TAG, "Buffered connection from port %u on %u", peer_port, (unsigned) state->buffered_port);

    // Kernel buffer stays at the TCP window; the deep staging is the ring.
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
    if (!buffered_ensure_ring(state)) {
      ESP_LOGE(TAG, "Failed to allocate buffered audio ring");
      close(client_sock);
      state->buffered_client_socket = -1;
      continue;
    }

    ByteRing ring;
    ring.attach(state->buffered_ring_storage, state->buffered_ring_capacity);
    size_t ring_peak = 0;
    uint32_t ring_full_passes = 0;
    TickType_t last_rx = xTaskGetTickCount();
    bool connection_open = true;

    while (stream->running && connection_open) {
      bool progressed = false;

      // Drain the socket whenever the ring has room, independent of the
      // pipeline, so the TCP window stays open.
      size_t space = 0;
      uint8_t *region = ring.write_region(&space);
      if (region) {
        const ssize_t n = recv(client_sock, region, space, MSG_DONTWAIT);
        if (n > 0) {
          ring.commit((size_t) n);
          ring_peak = ring.size() > ring_peak ? ring.size() : ring_peak;
          last_rx = xTaskGetTickCount();
          progressed = true;
        } else if (n == 0) {
          ESP_LOGI(TAG, "Buffered audio connection closed by peer");
          break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGE(TAG, "Buffered audio recv error: %d", errno);
          break;
        }
      } else {
        // Full: we are holding the sender back, not waiting on it.
        last_rx = xTaskGetTickCount();
        ring_full_passes++;
      }

      for (uint32_t i = 0; i < BUFFERED_FRAMES_PER_PASS && buffered_pipeline_has_room(state); i++) {
        size_t packet_len = 0;
        const BufferedFrameStatus status =
            buffered_ring_pop_frame(&ring, packet, BUFFERED_AUDIO_PACKET_SIZE, &packet_len);
        if (status == BufferedFrameStatus::INCOMPLETE) {
          break;
        }
        if (status == BufferedFrameStatus::INVALID) {
          ESP_LOGW(TAG, "Invalid buffered audio packet length: %u", (unsigned) packet_len);
          connection_open = false;
          break;
        }
        if (connection_packets++ == 0) {
          ESP_LOGI(TAG, "Buffered connection %u: first packet seq %u rtp %u", peer_port,
                   (unsigned) (((uint32_t) packet[1] << 16) | ((uint32_t) packet[2] << 8) | packet[3]),
                   (unsigned) (((uint32_t) packet[4] << 24) | ((uint32_t) packet[5] << 16) |
                               ((uint32_t) packet[6] << 8) | packet[7]));
        }
        buffered_process_frame(stream, state, packet, packet_len);
        progressed = true;
      }

      if (progressed) {
        continue;
      }
      if (state->timing.playing &&
          xTaskGetTickCount() - last_rx > pdMS_TO_TICKS(BUFFERED_IDLE_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Buffered audio timeout while playing");
        break;
      }
      if (region) {
        buffered_wait_readable(client_sock);
      } else {
        vTaskDelay(pdMS_TO_TICKS(BUFFERED_POLL_MS));
      }
    }

    ESP_LOGI(TAG, "Buffered connection %u closed after %u packets, ring peak %u/%u KB, %u full passes",
             peer_port, (unsigned) connection_packets, (unsigned) (ring_peak / 1024U),
             (unsigned) (ring.capacity() / 1024U), (unsigned) ring_full_passes);
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

  // shutdown(), not close(): the task still holds the descriptor and closes it
  // itself on the way out. Closing it here let that second close() land on
  // whatever socket had reused the number meanwhile.
  if (state->buffered_client_socket > 0) {
    shutdown(state->buffered_client_socket, SHUT_RDWR);
  }

  const bool stopped = buffered_wait_for_task_stopped(state, 20);

  // After the wait where possible: the task polls accept() on it until it exits.
  if (state->buffered_listen_socket > 0) {
    close(state->buffered_listen_socket);
    state->buffered_listen_socket = -1;
  }

  if (!stopped) {
    ESP_LOGW(TAG, "Buffered audio task did not exit within timeout");
    return;
  }

  if (state->buffered_recv_buffer) {
    airplay_free(state->buffered_recv_buffer);
    state->buffered_recv_buffer = nullptr;
  }
  if (state->buffered_ring_storage) {
    airplay_free(state->buffered_ring_storage);
    state->buffered_ring_storage = nullptr;
    state->buffered_ring_capacity = 0;
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
