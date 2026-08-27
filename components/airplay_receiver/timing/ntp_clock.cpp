// airplay_receiver NTP-style timing client (port of rbouteiller/airplay-esp32
// main/network/ntp_clock.c).
//
// UDP timing client used by the AirPlay 1 sync path. Sends a small 32-byte
// timing request to the sender every `kTimingIntervalMs`, and from each
// response derives the clock offset (remote_time = local_time + offset_ns)
// using the standard NTP filter; the lowest-dispersion measurement is kept as
// the running offset. A background FreeRTOS task owns the UDP socket; the whole
// thing is torn down by ntp_clock_stop().
//
// No heap is used (static state + a task-stack receive buffer), so there is
// nothing to route through airplay_*; ALL heap for this component goes through
// airplay_alloc/airplay_calloc/airplay_free in ../allocator.h.

#include "ntp_clock.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esphome/core/log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_ntp";

namespace {

// Timing packet types (from shairport-sync)
constexpr uint8_t kTimingRequest = 0xD2;
constexpr uint8_t kTimingResponse = 0xD3;

// Timing request packet structure (32 bytes)
struct __attribute__((packed)) TimingPacket {
  uint8_t leader;     // 0x80
  uint8_t type;       // 0xD2 for request
  uint16_t seqno;     // sequence number (network byte order)
  uint32_t filler;    // padding
  uint64_t origin;    // our transmit time (will be echoed back)
  uint64_t receive;   // zeroed in request
  uint64_t transmit;  // zeroed in request
};

// Number of measurements to keep for stability
constexpr int kTimingHistorySize = 8;
constexpr int kMinMeasurements = 3;
constexpr int kTimingIntervalMs = 3000;  // Send a request every 3 seconds

constexpr uint32_t kNtpStackSize = 3072;

// Timing state (static, no heap)
struct NtpState {
  bool running{false};
  TaskHandle_t task_handle{nullptr};
  int socket{-1};
  sockaddr_in remote_addr{};

  // Clock offset tracking
  bool locked{false};
  int64_t offset_ns{0};  // Current best offset
  int64_t measurements[kTimingHistorySize]{};
  int64_t dispersions[kTimingHistorySize]{};
  int measurement_count{0};
  int measurement_index{0};
};

NtpState ntp;

// Cross-task publication of the best NTP offset. ntp_task WRITES it; the
// RTSP task (audio_timing_set_anchor) and playback task read it. int64 is two
// 32-bit words on Xtensa, so a plain write/read is torn (spurious ±4.29 s step).
// Publish through a 32-bit seqlock (lock-free, IRAM-safe, no libatomic).
static volatile uint32_t g_ntp_offset_seq = 0U;
static volatile uint32_t g_ntp_offset_lo = 0U;
static volatile uint32_t g_ntp_offset_hi = 0U;

static void ntp_publish_offset(int64_t off) {
  const uint32_t s = g_ntp_offset_seq;
  g_ntp_offset_seq = s + 1U;
  const uint64_t u = (uint64_t) off;
  g_ntp_offset_lo = (uint32_t) u;
  g_ntp_offset_hi = (uint32_t) (u >> 32);
  g_ntp_offset_seq = s + 2U;
}

static int64_t ntp_read_offset(void) {
  uint32_t s0, s1, lo, hi;
  do {
    s0 = g_ntp_offset_seq;
    lo = g_ntp_offset_lo;
    hi = g_ntp_offset_hi;
    s1 = g_ntp_offset_seq;
  } while (s0 != s1 || (s0 & 1U) != 0U);
  return (int64_t) (((uint64_t) hi << 32) | (uint64_t) lo);
}

// Convert local time (microseconds) to NTP timestamp format (for packet)
uint64_t local_us_to_ntp(int64_t local_us) {
  // NTP timestamp: upper 32 bits = seconds, lower 32 bits = fraction
  uint32_t secs = static_cast<uint32_t>(local_us / 1000000);
  uint64_t frac_us = local_us % 1000000;
  uint32_t frac = static_cast<uint32_t>((frac_us << 32) / 1000000);
  return (static_cast<uint64_t>(secs) << 32) | frac;
}

// Convert NTP timestamp from packet to nanoseconds
int64_t ntp_to_ns(uint32_t secs, uint32_t frac) {
  int64_t ns = static_cast<int64_t>(secs) * 1000000000LL;
  ns += (static_cast<int64_t>(frac) * 1000000000LL) >> 32;
  return ns;
}

// Process timing response and calculate offset
void process_timing_response(const uint8_t *packet, size_t len, int64_t arrival_ns) {
  if (len < sizeof(TimingPacket)) {
    return;
  }

  // Extract timestamps from response.
  // Origin: our original transmit time (echoed back)
  // Receive: when remote received our request
  // Transmit: when remote sent response
  //
  // Offsets in packet (after 8-byte header)
  // Bytes 8-15: origin timestamp (our transmit time, echoed)
  // Bytes 16-23: receive timestamp (remote's receive time)
  // Bytes 24-31: transmit timestamp (remote's transmit time)

  uint32_t origin_secs = ntohl(*(uint32_t *) (packet + 8));
  uint32_t origin_frac = ntohl(*(uint32_t *) (packet + 12));
  uint32_t receive_secs = ntohl(*(uint32_t *) (packet + 16));
  uint32_t receive_frac = ntohl(*(uint32_t *) (packet + 20));
  uint32_t transmit_secs = ntohl(*(uint32_t *) (packet + 24));
  uint32_t transmit_frac = ntohl(*(uint32_t *) (packet + 28));

  int64_t departure_ns = ntp_to_ns(origin_secs, origin_frac);
  int64_t remote_receive_ns = ntp_to_ns(receive_secs, receive_frac);
  int64_t remote_transmit_ns = ntp_to_ns(transmit_secs, transmit_frac);

  // Calculate offset using the NTP formula:
  //   Round-trip time = (arrival - departure) - (transmit - receive)
  //   Offset = ((receive - departure) + (transmit - arrival)) / 2
  //          = remote_transmit + (return_time - remote_processing) / 2 - arrival
  int64_t round_trip_ns = arrival_ns - departure_ns;
  int64_t remote_processing_ns = remote_transmit_ns - remote_receive_ns;
  int64_t network_delay_ns = round_trip_ns - remote_processing_ns;

  // Offset = remote_transmit + network_delay/2 - arrival
  // This gives: remote_time = local_time + offset
  int64_t offset_ns = remote_transmit_ns + (network_delay_ns / 2) - arrival_ns;

  // Dispersion is the uncertainty (half round-trip time)
  int64_t dispersion_ns = network_delay_ns / 2;

  // Sanity check: reject if round-trip is negative or too large (>1 second)
  if (round_trip_ns < 0 || round_trip_ns > 1000000000LL) {
    ESP_LOGW(TAG, "Rejecting measurement: RTT=%lld ms", static_cast<long long>(round_trip_ns / 1000000));
    return;
  }

  // Store measurement
  int idx = ntp.measurement_index;
  ntp.measurements[idx] = offset_ns;
  ntp.dispersions[idx] = dispersion_ns;
  ntp.measurement_index = (idx + 1) % kTimingHistorySize;
  if (ntp.measurement_count < kTimingHistorySize) {
    ntp.measurement_count++;
  }

  // Find best measurement (lowest dispersion)
  int64_t best_offset = offset_ns;
  int64_t best_dispersion = dispersion_ns;
  for (int i = 0; i < ntp.measurement_count; i++) {
    if (ntp.dispersions[i] < best_dispersion) {
      best_dispersion = ntp.dispersions[i];
      best_offset = ntp.measurements[i];
    }
  }

  ntp.offset_ns = best_offset;
  ntp_publish_offset(best_offset);

  // Consider locked after enough measurements
  if (!ntp.locked && ntp.measurement_count >= kMinMeasurements) {
    ntp.locked = true;
    ESP_LOGI(TAG, "NTP timing locked: offset=%lld ms, dispersion=%lld us",
             static_cast<long long>(ntp.offset_ns / 1000000), static_cast<long long>(best_dispersion / 1000));
  }

  ESP_LOGD(TAG, "Timing: RTT=%lld us, offset=%lld ms", static_cast<long long>(round_trip_ns / 1000),
           static_cast<long long>(offset_ns / 1000000));
}

// Send timing request
void send_timing_request() {
  TimingPacket req;
  memset(&req, 0, sizeof(req));

  req.leader = 0x80;
  req.type = kTimingRequest;
  req.seqno = htons(7);  // Fixed sequence number (like shairport-sync)

  // Set transmit field to our current time — the AirPlay source echoes
  // request bytes 24-31 (transmit) into response bytes 8-15 (origin).
  int64_t now_us = esp_timer_get_time();
  uint64_t ntp_time = local_us_to_ntp(now_us);

  // Split into network byte order (avoid unaligned access)
  uint8_t *tx_bytes = reinterpret_cast<uint8_t *>(&req.transmit);
  uint32_t secs = htonl(static_cast<uint32_t>(ntp_time >> 32));
  uint32_t frac = htonl(static_cast<uint32_t>(ntp_time & 0xFFFFFFFF));
  memcpy(tx_bytes, &secs, 4);
  memcpy(tx_bytes + 4, &frac, 4);

  sendto(ntp.socket, &req, sizeof(req), 0, reinterpret_cast<sockaddr *>(&ntp.remote_addr),
         sizeof(ntp.remote_addr));
}

// Timing task: sends requests and processes responses
void ntp_task(void *pvParameters) {
  (void) pvParameters;
  uint8_t packet[64];
  sockaddr_in src_addr;
  socklen_t addr_len;

  // Initial delay before first request
  vTaskDelay(pdMS_TO_TICKS(300));

  TickType_t last_request = 0;

  while (ntp.running) {
    // Send timing request periodically
    TickType_t now = xTaskGetTickCount();
    if (now - last_request >= pdMS_TO_TICKS(kTimingIntervalMs)) {
      send_timing_request();
      last_request = now;
    }

    // Check for response (with short timeout)
    addr_len = sizeof(src_addr);
    ssize_t len = recvfrom(ntp.socket, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&src_addr), &addr_len);

    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      if (ntp.running) {
        ESP_LOGE(TAG, "recvfrom error: %d", errno);
      }
      break;
    }

    if (len >= 2) {
      int64_t arrival_ns = esp_timer_get_time() * 1000LL;
      uint8_t pkt_type = packet[1];

      if (pkt_type == kTimingResponse) {
        process_timing_response(packet, len, arrival_ns);
      }
    }
  }

  ntp.task_handle = nullptr;
  vTaskDelete(nullptr);
}

bool ntp_wait_for_task_stopped(int timeout_ticks) {
  while (ntp.task_handle != nullptr && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return ntp.task_handle == nullptr;
}

}  // namespace

esp_err_t ntp_clock_start_client(uint32_t remote_ip, uint16_t remote_port) {
  if (ntp.running) {
    // If already running to same target, keep going
    if (ntp.remote_addr.sin_addr.s_addr == remote_ip && ntohs(ntp.remote_addr.sin_port) == remote_port) {
      return ESP_OK;
    }
    // Stop existing and restart with new target
    ntp_clock_stop();
    if (!ntp_wait_for_task_stopped(20)) {
      ESP_LOGE(TAG, "Previous NTP task did not stop");
      return ESP_ERR_INVALID_STATE;
    }
  }
  if (ntp.task_handle != nullptr) {
    ESP_LOGW(TAG, "NTP task still stopping, waiting");
    if (!ntp_wait_for_task_stopped(20)) {
      ESP_LOGE(TAG, "NTP task still active");
      return ESP_ERR_INVALID_STATE;
    }
  }

  ntp.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (ntp.socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return ESP_FAIL;
  }

  // Set receive timeout (100ms for responsive checking)
  struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
  setsockopt(ntp.socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Setup remote address
  memset(&ntp.remote_addr, 0, sizeof(ntp.remote_addr));
  ntp.remote_addr.sin_family = AF_INET;
  ntp.remote_addr.sin_addr.s_addr = remote_ip;
  ntp.remote_addr.sin_port = htons(remote_port);

  // Reset state
  ntp.locked = false;
  ntp.offset_ns = 0;
  ntp_publish_offset(0);
  ntp.measurement_count = 0;
  ntp.measurement_index = 0;
  memset(ntp.measurements, 0, sizeof(ntp.measurements));
  memset(ntp.dispersions, 0, sizeof(ntp.dispersions));
  ntp.running = true;

  ntp.task_handle = nullptr;
  BaseType_t task_ret = xTaskCreate(ntp_task, "ntp_clock", kNtpStackSize, nullptr, 5, &ntp.task_handle);
  if (task_ret != pdPASS || ntp.task_handle == nullptr) {
    ESP_LOGE(TAG, "Failed to create NTP task");
    close(ntp.socket);
    ntp.socket = -1;
    ntp.running = false;
    return ESP_FAIL;
  }

  uint8_t *ip = reinterpret_cast<uint8_t *>(&remote_ip);
  ESP_LOGI(TAG, "NTP timing client started -> %d.%d.%d.%d:%d", ip[0], ip[1], ip[2], ip[3], remote_port);
  return ESP_OK;
}

void ntp_clock_stop(void) {
  if (!ntp.running) {
    return;
  }

  ntp.running = false;

  if (ntp.socket >= 0) {
    close(ntp.socket);
    ntp.socket = -1;
  }

  if (ntp.task_handle) {
    if (!ntp_wait_for_task_stopped(20)) {
      ESP_LOGW(TAG, "NTP task did not exit within timeout");
    }
  }

  ntp.locked = false;
  ntp.measurement_count = 0;
  ESP_LOGI(TAG, "NTP timing stopped");
}

bool ntp_clock_is_locked(void) {
  return ntp.locked;
}

int64_t ntp_clock_get_offset_ns(void) {
  return ntp_read_offset();
}

}  // namespace airplay_receiver
}  // namespace esphome
