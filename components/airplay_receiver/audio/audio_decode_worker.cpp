// airplay_receiver FreeRTOS audio decode worker (C++ port of
// rbouteiller/airplay-esp32 main/audio/audio_decode_worker.c).
//
// A pointer-job queue plus a dedicated FreeRTOS task.  The ingress tasks
// (realtime / buffered) allocate a job, deep-copy the (already decrypted)
// compressed access unit into it, and enqueue the pointer; this task dequeues
// and runs audio_stream_decode_encoded_packet() -> audio_decoder_decode() +
// audio_engine_v2_push_pcm_wait().  Unpinning the decoder off the receive tasks
// keeps bulk AAC/ALAC decode from stalling socket reads or the I2S refill.
//
// SHARED CONTRACT:
//   * namespace esphome::airplay_receiver
//   * ALL heap via airplay_alloc / airplay_free (../allocator.h)
//   * logs via esphome/core/log.h + file-local TAG
//   * upstream signatures preserved 1:1

#include "audio_decode_worker.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"

#include "esphome/core/log.h"

#include "audio_epoch.h"  // audio_epoch_matches
#include "audio_receiver_internal.h"
#include "audio_stream.h"

#include "../allocator.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "audio_decode";

#define AUDIO_DECODE_QUEUE_DEPTH 16U
#define AUDIO_DECODE_MAX_PAYLOAD 8192U
#define AUDIO_DECODE_TASK_STACK 6144U

/* Sits above the buffered TCP reader (5) so decoded PCM keeps draining ahead of
 * ingress, and below the RTP/control receivers (7/8) and the I2S playback task
 * (9) so bulk AAC decoding can never delay them.  Deliberately not pinned: the
 * AAC decode it takes over used to run inline on the unpinned "buff_audio"
 * task, and on ESP32 core 0 is already shared with WiFi, BT and lwIP. */
#define AUDIO_DECODE_TASK_PRIORITY 6

// Same core as the playback task (audio_output.cpp) so the whole audio path
// stays off core 0, which serves WiFi and the ESPHome main loop.
#if CONFIG_FREERTOS_UNICORE
#define AUDIO_DECODE_TASK_CORE 0
#else
#define AUDIO_DECODE_TASK_CORE 1
#endif

// Worker handle (opaque outside this file).  Matches the upstream layout: the
// state pointer, the job pointer queue, the task handle and the two volatile
// generation/running flags.  Defined in the named namespace so it matches the
// header's `typedef struct audio_decode_worker audio_decode_worker_t;`.
struct audio_decode_worker {
  audio_receiver_state_t *state;
  QueueHandle_t queue;
  TaskHandle_t task;
  volatile bool running;
  volatile uint32_t cancel_generation;
};

namespace {

// One queued decode job.  `payload` is a flexible array member holding the
// deep-copied (already decrypted) access unit; it is owned by the job and freed
// in the decode task.  `generation` records the flush epoch at enqueue time so
// a discard can invalidate in-flight work without touching a shared slot pool.
typedef struct audio_decode_job {
  uint32_t generation;
  audio_encoded_packet_t packet;
  uint8_t payload[];
} audio_decode_job_t;

audio_decode_job_t *job_alloc(size_t payload_len) {
  if (payload_len == 0U || payload_len > AUDIO_DECODE_MAX_PAYLOAD ||
      payload_len > SIZE_MAX - sizeof(audio_decode_job_t)) {
    return NULL;
  }

  // Upstream preferred PSRAM-first (8 KiB-capable) for the payload, falling
  // back to internal malloc.  airplay_alloc(size, realtime=false) is the
  // PSRAM-first profile (with the <= AIRPLAY_ALWAYS_INTERNAL_BYTES exception
  // keeping small codec frames in internal DRAM).
  const size_t bytes = sizeof(audio_decode_job_t) + payload_len;
  return static_cast<audio_decode_job_t *>(airplay_alloc(bytes, false));
}

void job_free(audio_decode_job_t *job) { airplay_free(job); }

void decode_task(void *arg) {
  audio_decode_worker_t *worker = static_cast<audio_decode_worker_t *>(arg);

  for (;;) {
    audio_decode_job_t *job = NULL;
    if (xQueueReceive(worker->queue, &job, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    /* NULL is the shutdown sentinel. */
    if (!job) {
      if (!worker->running) {
        break;
      }
      continue;
    }

    (void) __atomic_add_fetch(&worker->state->engine_v2.diag_dequeued, 1U,
                              __ATOMIC_RELAXED);

    const uint32_t current_generation =
        __atomic_load_n(&worker->cancel_generation, __ATOMIC_ACQUIRE);

    /* Each queued job owns its payload and records the generation at enqueue
     * time.  A flush invalidates all old jobs without touching any shared slot
     * pool, so ownership cannot be lost between two queues. */
    if (worker->running && job->generation == current_generation &&
        audio_epoch_matches(&worker->state->engine_v2.epoch,
                            job->packet.epoch)) {
      // DECODE + TIMELINE: audio_stream_decode_encoded_packet() runs, under the
      // decoder mutex, audio_decoder_decode(state->decoder, ...) and then
      // audio_engine_v2_deferred_flush() + audio_engine_v2_push_pcm_wait() to
      // push the decoded PCM into the engine v2 timeline.
      if (!audio_stream_decode_encoded_packet(worker->state, &job->packet)) {
        worker->state->stats.packets_dropped++;
      }
    } else {
      worker->state->stats.packets_dropped++;
      (void) __atomic_add_fetch(&worker->state->engine_v2.diag_epoch_drops, 1U,
                                __ATOMIC_RELAXED);
    }

    job_free(job);
  }

  worker->task = NULL;
  vTaskDelete(NULL);
}

}  // namespace

esp_err_t audio_decode_worker_create(audio_receiver_state_t *state,
                                     audio_decode_worker_t **out_worker) {
  if (!state || !out_worker) {
    return ESP_ERR_INVALID_ARG;
  }

  audio_decode_worker_t *worker = static_cast<audio_decode_worker_t *>(
      airplay_calloc(1, sizeof(*worker), false));
  if (!worker) {
    return ESP_ERR_NO_MEM;
  }
  worker->state = state;
  worker->queue =
      xQueueCreate(AUDIO_DECODE_QUEUE_DEPTH, sizeof(audio_decode_job_t *));
  if (!worker->queue) {
    airplay_free(worker);
    return ESP_ERR_NO_MEM;
  }

  worker->running = true;
  // Pin decode to the audio core. Left unpinned it floats onto core 0, where
  // it outranks the ESPHome main loop (priority 1) and starves it: the API
  // connection drops with EOF, OTA handshakes time out mid-stream, and decode
  // ends up contending with the WiFi stack for the same core.
  BaseType_t ok = xTaskCreatePinnedToCore(decode_task, "audio_decode", AUDIO_DECODE_TASK_STACK, worker,
                                          AUDIO_DECODE_TASK_PRIORITY, &worker->task, AUDIO_DECODE_TASK_CORE);
  if (ok != pdPASS || !worker->task) {
    worker->running = false;
    vQueueDelete(worker->queue);
    airplay_free(worker);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(
      TAG,
      "DQ1 pointer-job queue ready: depth=%u max_payload=%u ownership=job",
      (unsigned) AUDIO_DECODE_QUEUE_DEPTH, (unsigned) AUDIO_DECODE_MAX_PAYLOAD);
  *out_worker = worker;
  return ESP_OK;
}

void audio_decode_worker_discard_pending(audio_decode_worker_t *worker) {
  if (!worker || !worker->queue) {
    return;
  }

  /* Invalidate a job that the decoder task may already have removed from the
   * queue, then free every job that is still queued.  There is no reusable
   * slot/free-queue accounting to reconstruct after a flush. */
  const uint32_t generation =
      __atomic_add_fetch(&worker->cancel_generation, 1U, __ATOMIC_ACQ_REL);

  size_t discarded = 0U;
  audio_decode_job_t *job = NULL;
  while (xQueueReceive(worker->queue, &job, 0) == pdTRUE) {
    if (job) {
      job_free(job);
      discarded++;
    }
  }

  if (discarded > 0U) {
    ESP_LOGI(TAG, "DQ1 flush: generation=%lu discarded=%u",
             (unsigned long) generation, (unsigned) discarded);
  }
}

void audio_decode_worker_destroy(audio_decode_worker_t *worker) {
  if (!worker) {
    return;
  }

  if (worker->queue) {
    audio_decode_worker_discard_pending(worker);
  }

  if (worker->task) {
    worker->running = false;
    audio_decode_job_t *stop = NULL;
    (void) xQueueSend(worker->queue, &stop, 0);
    for (int i = 0; i < 100 && worker->task; ++i) {
      vTaskDelay(1);
    }
  }

  if (worker->task) {
    ESP_LOGE(TAG, "DQ1 destroy timeout: decoder task still running");
    /* Do not delete the queue or free worker memory underneath a live task. */
    return;
  }

  if (worker->queue) {
    audio_decode_job_t *job = NULL;
    while (xQueueReceive(worker->queue, &job, 0) == pdTRUE) {
      if (job) {
        job_free(job);
      }
    }
    vQueueDelete(worker->queue);
  }
  airplay_free(worker);
}

audio_decode_enqueue_result_t audio_decode_worker_enqueue(
    audio_decode_worker_t *worker, const audio_encoded_packet_t *packet,
    uint32_t timeout_ms) {
  if (!worker || !worker->running || !worker->queue || !packet ||
      !packet->payload || packet->payload_len == 0U ||
      packet->payload_len > AUDIO_DECODE_MAX_PAYLOAD) {
    return AUDIO_DECODE_ENQUEUE_DROP;
  }

  const uint32_t generation =
      __atomic_load_n(&worker->cancel_generation, __ATOMIC_ACQUIRE);

  if (!audio_epoch_matches(&worker->state->engine_v2.epoch, packet->epoch)) {
    return AUDIO_DECODE_ENQUEUE_DROP;
  }

  audio_decode_job_t *job = job_alloc(packet->payload_len);
  if (!job) {
    ESP_LOGW(TAG, "DQ1 allocation drop: payload=%u pending=%u",
             (unsigned) packet->payload_len,
             (unsigned) uxQueueMessagesWaiting(worker->queue));
    return AUDIO_DECODE_ENQUEUE_DROP;
  }

  job->generation = generation;
  job->packet = *packet;
  memcpy(job->payload, packet->payload, packet->payload_len);
  job->packet.payload = job->payload;

  /* Close the race with a concurrent flush after allocation/copy. */
  if (!worker->running ||
      generation !=
          __atomic_load_n(&worker->cancel_generation, __ATOMIC_ACQUIRE) ||
      !audio_epoch_matches(&worker->state->engine_v2.epoch, packet->epoch)) {
    job_free(job);
    return AUDIO_DECODE_ENQUEUE_DROP;
  }

  TickType_t timeout = timeout_ms == 0U ? 0 : pdMS_TO_TICKS(timeout_ms);
  if (timeout_ms > 0U && timeout == 0) {
    timeout = 1;
  }
  if (xQueueSend(worker->queue, &job, timeout) != pdTRUE) {
    job_free(job);
    return AUDIO_DECODE_ENQUEUE_RETRY;
  }

  return AUDIO_DECODE_ENQUEUE_OK;
}

size_t audio_decode_worker_pending(const audio_decode_worker_t *worker) {
  if (!worker || !worker->queue) {
    return 0;
  }
  return static_cast<size_t>(uxQueueMessagesWaiting(worker->queue));
}

bool audio_decode_worker_is_nearly_full(const audio_decode_worker_t *worker) {
  return audio_decode_worker_pending(worker) >= (AUDIO_DECODE_QUEUE_DEPTH - 4U);
}

}  // namespace airplay_receiver
}  // namespace esphome
