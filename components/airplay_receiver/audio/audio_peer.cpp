// Stereo pair link. See audio_peer.h for the design and peer_protocol.h for
// the wire format.
#include "audio_peer.h"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <new>

#include "esphome/core/log.h"

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "../allocator.h"
#include "../timing/ptp_clock.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "audio_timeline.h"
#include "peer_clock.h"
#include "peer_protocol.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_peer";

// Primary: a STATE goes out on any change, found by polling this often, and
// at least this often regardless so a lost one is repaired.
static constexpr uint32_t STATE_POLL_MS = 10;
static constexpr int64_t STATE_HEARTBEAT_US = 250000;
// Forwarded blocks the primary can hold while the link catches up: ~3 s of
// AAC. A buffered stream's opening burst is paced by the decoder (~3 ms a
// frame), far slower than the link drains it.
static constexpr uint32_t RING_SLOTS = 128;
// Yield after this many datagrams in a row, so a backlog cannot monopolise
// the Wi-Fi TX queue or the core.
static constexpr int DATAGRAMS_PER_YIELD = 16;
static constexpr int SEND_RETRIES = 5;

// Secondary.
static constexpr uint32_t RX_TIMEOUT_MS = 50;
static constexpr int64_t LINK_TIMEOUT_US = 1500000;
// A full timeline means the secondary is behind by more than the primary
// would ever queue; waiting longer than a frame or two only backs the socket
// up.
static constexpr uint32_t PUSH_TIMEOUT_MS = 40;
static constexpr uint32_t PING_INTERVAL_MS = 100;

static constexpr int64_t STATS_INTERVAL_US = 5000000;

static constexpr uint32_t DATA_TASK_STACK = 4096;
static constexpr uint32_t CLOCK_TASK_STACK = 3072;
static constexpr UBaseType_t DATA_TASK_PRIORITY = 6;
// Above the playback task (9): a clock exchange is a few microseconds of
// work, and a timestamp read late is clock error.
static constexpr UBaseType_t CLOCK_TASK_PRIORITY = 10;
static constexpr BaseType_t PEER_CORE = 0;

// Clock exchange buffers, both roles.
static uint8_t *g_clock_rx_buffer = nullptr;
static uint8_t *g_clock_tx_buffer = nullptr;

enum class ClockSource : uint8_t { NONE = 0, PTP, RELAY };

static const char *clock_source_name(ClockSource source) {
  switch (source) {
    case ClockSource::PTP:
      return "ptp";
    case ClockSource::RELAY:
      return "relay";
    default:
      return "none";
  }
}

static const char *anchor_domain_name(peer::AnchorDomain domain) {
  switch (domain) {
    case peer::AnchorDomain::PTP:
      return "ptp";
    case peer::AnchorDomain::NTP:
      return "ntp";
    case peer::AnchorDomain::LOCAL:
      return "local";
    default:
      return "none";
  }
}

static PeerConfig g_config;
static std::atomic<PeerRole> g_role{PeerRole::STANDALONE};
static uint32_t g_boot_id = 0;

static int64_t local_now_ns() { return esp_timer_get_time() * 1000LL; }

static int open_udp(uint16_t bind_port, uint32_t recv_timeout_ms) {
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    return -1;
  }
  if (bind_port != 0) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bind_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      ESP_LOGE(TAG, "bind(:%u) failed: errno %d", (unsigned) bind_port, errno);
      close(fd);
      return -1;
    }
  }
  if (recv_timeout_ms > 0) {
    timeval tv{};
    tv.tv_sec = recv_timeout_ms / 1000;
    tv.tv_usec = (recv_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  return fd;
}

static sockaddr_in peer_addr(uint32_t ip, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = ip;
  return addr;
}

// Retries a full TX queue briefly; anything else is a drop.
static bool send_datagram(int fd, const sockaddr_in &to, const uint8_t *data, size_t length) {
  for (int attempt = 0; attempt < SEND_RETRIES; attempt++) {
    const int sent = sendto(fd, data, length, 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    if (sent == (int) length) {
      return true;
    }
    if (errno != ENOMEM && errno != EAGAIN) {
      return false;
    }
    vTaskDelay(1);
  }
  return false;
}

// ===========================================================================
// Primary
// ===========================================================================

struct RingSlot {
  volatile bool ready;
  uint32_t epoch;
  uint32_t rtp;
  uint32_t flush_serial;
  uint32_t flush_rtp;
  uint16_t samples;
  uint8_t channels;
  int16_t *pcm;
};

static RingSlot *g_ring = nullptr;
// Monotonic counters; the slot is counter % RING_SLOTS. Only the taps advance
// head and only the primary task advances tail.
static uint32_t g_ring_head = 0;
static uint32_t g_ring_tail = 0;
static portMUX_TYPE g_ring_lock = portMUX_INITIALIZER_UNLOCKED;
// Deferred flushes on the primary, carried by every later block.
static uint32_t g_flush_serial = 0;
static uint32_t g_flush_rtp = 0;
static TaskHandle_t g_primary_task = nullptr;

struct PrimaryStats {
  uint32_t blocks_sent;
  uint32_t datagrams_sent;
  uint32_t send_errors;
  uint32_t ring_full_drops;
  uint32_t ring_high_water;
  uint32_t states_sent;
  uint32_t pings;
};
static PrimaryStats g_primary_stats{};

static uint8_t forward_channels() { return g_config.forward == PeerForward::STEREO ? 2 : 1; }

void audio_peer_tap_pcm(uint32_t epoch, uint32_t first_rtp, const int16_t *pcm, size_t samples, uint8_t channels) {
  if (g_role.load(std::memory_order_relaxed) != PeerRole::PRIMARY || g_ring == nullptr || pcm == nullptr ||
      samples == 0 || samples > AUDIO_PCM_BLOCK_MAX_SAMPLES || channels == 0) {
    return;
  }

  portENTER_CRITICAL(&g_ring_lock);
  const uint32_t queued = g_ring_head - g_ring_tail;
  if (queued >= RING_SLOTS) {
    g_primary_stats.ring_full_drops++;
    portEXIT_CRITICAL(&g_ring_lock);
    return;
  }
  RingSlot *slot = &g_ring[g_ring_head % RING_SLOTS];
  slot->ready = false;
  g_ring_head++;
  if (queued + 1 > g_primary_stats.ring_high_water) {
    g_primary_stats.ring_high_water = queued + 1;
  }
  slot->flush_serial = g_flush_serial;
  slot->flush_rtp = g_flush_rtp;
  portEXIT_CRITICAL(&g_ring_lock);

  // The slot is reserved and not ready, so the copy runs unlocked.
  slot->epoch = epoch;
  slot->rtp = first_rtp;
  slot->samples = (uint16_t) samples;
  const uint8_t out_channels = forward_channels();
  slot->channels = out_channels;
  int16_t *out = slot->pcm;
  for (size_t i = 0; i < samples; i++) {
    const int16_t left = pcm[i * channels];
    const int16_t right = channels > 1 ? pcm[i * channels + 1] : left;
    switch (g_config.forward) {
      case PeerForward::LEFT:
        out[i] = left;
        break;
      case PeerForward::RIGHT:
        out[i] = right;
        break;
      case PeerForward::MONO:
        out[i] = (int16_t) (((int32_t) left + (int32_t) right) / 2);
        break;
      case PeerForward::STEREO:
        out[2 * i] = left;
        out[2 * i + 1] = right;
        break;
    }
  }
  __atomic_store_n(&slot->ready, true, __ATOMIC_RELEASE);
  if (g_primary_task != nullptr) {
    xTaskNotifyGive(g_primary_task);
  }
}

void audio_peer_tap_deferred_flush(uint32_t epoch, uint32_t flush_rtp) {
  (void) epoch;
  if (g_role.load(std::memory_order_relaxed) != PeerRole::PRIMARY) {
    return;
  }
  portENTER_CRITICAL(&g_ring_lock);
  g_flush_serial++;
  g_flush_rtp = flush_rtp;
  portEXIT_CRITICAL(&g_ring_lock);
  if (g_primary_task != nullptr) {
    xTaskNotifyGive(g_primary_task);
  }
}

static bool state_equal(const peer::State &a, const peer::State &b) {
  return a.engine_ready == b.engine_ready && a.epoch == b.epoch && a.flush_serial == b.flush_serial &&
         a.flush_rtp == b.flush_rtp && a.sample_rate == b.sample_rate && a.frame_samples == b.frame_samples &&
         a.playing == b.playing && a.output_running == b.output_running && a.anchor_valid == b.anchor_valid &&
         a.domain == b.domain && a.anchor_rtp == b.anchor_rtp && a.anchor_network_ns == b.anchor_network_ns &&
         a.playout_offset_ns == b.playout_offset_ns && a.clock_id == b.clock_id && a.volume_q15 == b.volume_q15;
}

static void primary_snapshot(peer::State *state) {
  *state = peer::State{};
  audio_receiver_peer_snapshot(state);
  portENTER_CRITICAL(&g_ring_lock);
  state->flush_serial = g_flush_serial;
  state->flush_rtp = g_flush_rtp;
  portEXIT_CRITICAL(&g_ring_lock);
  state->output_running = audio_output_is_running();
  state->volume_q15 = audio_output_get_volume_q15();
  state->clock_id = state->domain == peer::AnchorDomain::PTP ? ptp_clock_get_master_clock_id() : 0;
}

// Datagram buffers, allocated by the role that needs them so a standalone
// board pays nothing for the link.
static uint8_t *g_tx_buffer = nullptr;
static uint32_t g_tx_seq = 0;

static void send_slot(int fd, const sockaddr_in &to, const RingSlot &slot, int *burst) {
  const size_t per_chunk = peer::pcm_chunk_capacity(slot.channels);
  peer::PcmChunk chunk{};
  chunk.epoch = slot.epoch;
  chunk.rtp = slot.rtp;
  chunk.flush_serial = slot.flush_serial;
  chunk.flush_rtp = slot.flush_rtp;
  chunk.frame_samples = slot.samples;
  chunk.channels = slot.channels;
  for (size_t offset = 0; offset < slot.samples; offset += per_chunk) {
    const size_t count = slot.samples - offset < per_chunk ? slot.samples - offset : per_chunk;
    chunk.offset = (uint16_t) offset;
    chunk.count = (uint16_t) count;
    const size_t length = peer::encode_pcm(g_tx_buffer, peer::MAX_DATAGRAM, g_boot_id, g_tx_seq++, chunk,
                                           &slot.pcm[offset * slot.channels]);
    if (length == 0 || !send_datagram(fd, to, g_tx_buffer, length)) {
      g_primary_stats.send_errors++;
    } else {
      g_primary_stats.datagrams_sent++;
    }
    if (++*burst >= DATAGRAMS_PER_YIELD) {
      *burst = 0;
      vTaskDelay(1);
    }
  }
  g_primary_stats.blocks_sent++;
}

static void send_state(int fd, const sockaddr_in &to, const peer::State &state, int64_t now_us, peer::State *last,
                       bool *sent_once, int64_t *last_state_us) {
  const size_t length = peer::encode_state(g_tx_buffer, peer::MAX_DATAGRAM, g_boot_id, g_tx_seq++, state);
  if (length > 0 && send_datagram(fd, to, g_tx_buffer, length)) {
    g_primary_stats.states_sent++;
    *last = state;
    *sent_once = true;
    *last_state_us = now_us;
  }
}

static void primary_task(void *arg) {
  (void) arg;
  const int fd = open_udp(0, 0);
  if (fd < 0) {
    vTaskDelete(nullptr);
    return;
  }
  const sockaddr_in to = peer_addr(g_config.peer_ip, g_config.port);
  peer::State last{};
  bool sent_once = false;
  int64_t last_state_us = 0;
  int64_t last_stats_us = esp_timer_get_time();

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATE_POLL_MS));
    const int64_t now_us = esp_timer_get_time();

    peer::State state;
    primary_snapshot(&state);
    if (!sent_once || !state_equal(state, last) || now_us - last_state_us >= STATE_HEARTBEAT_US) {
      send_state(fd, to, state, now_us, &last, &sent_once, &last_state_us);
    }

    int burst = 0;
    for (;;) {
      portENTER_CRITICAL(&g_ring_lock);
      const bool empty = g_ring_tail == g_ring_head;
      RingSlot *slot = empty ? nullptr : &g_ring[g_ring_tail % RING_SLOTS];
      portEXIT_CRITICAL(&g_ring_lock);
      if (slot == nullptr || !__atomic_load_n(&slot->ready, __ATOMIC_ACQUIRE)) {
        break;
      }
      // The first block of a new epoch can follow a new frame length or rate.
      // Describe them first, or the secondary slots the block into the old
      // layout and loses it at the re-cut the late STATE then forces.
      if (slot->epoch != last.epoch || !sent_once) {
        primary_snapshot(&state);
        send_state(fd, to, state, esp_timer_get_time(), &last, &sent_once, &last_state_us);
      }
      // A block from an epoch the primary has already left is gone from its
      // own timeline too; the secondary would only drop it.
      if (!sent_once || !peer::serial_newer(last.epoch, slot->epoch)) {
        send_slot(fd, to, *slot, &burst);
      }
      portENTER_CRITICAL(&g_ring_lock);
      g_ring_tail++;
      portEXIT_CRITICAL(&g_ring_lock);
    }

    if (now_us - last_stats_us >= STATS_INTERVAL_US) {
      portENTER_CRITICAL(&g_ring_lock);
      const PrimaryStats stats = g_primary_stats;
      g_primary_stats = PrimaryStats{};
      const uint32_t queued = g_ring_head - g_ring_tail;
      portEXIT_CRITICAL(&g_ring_lock);
      // Quiet while idle: only heartbeats went out.
      if (stats.blocks_sent > 0 || stats.ring_full_drops > 0 || stats.send_errors > 0) {
        ESP_LOGI(TAG,
                 "peer tx: blocks=%" PRIu32 " datagrams=%" PRIu32 " send_err=%" PRIu32 " ring_full=%" PRIu32
                 " queue=%" PRIu32 " hw=%" PRIu32 " states=%" PRIu32 " pings=%" PRIu32 " anchor=%s",
                 stats.blocks_sent, stats.datagrams_sent, stats.send_errors, stats.ring_full_drops, queued,
                 stats.ring_high_water, stats.states_sent, stats.pings, anchor_domain_name(last.domain));
      }
      last_stats_us = now_us;
    }
  }
}

// Answers PINGs with the primary's network time. Blocks in recvfrom() so the
// receive timestamp is read as soon as the datagram is handed over.
static void primary_clock_task(void *arg) {
  (void) arg;
  const int fd = open_udp(g_config.port + 1, 0);
  if (fd < 0) {
    vTaskDelete(nullptr);
    return;
  }
  uint8_t *buffer = g_clock_rx_buffer;
  uint8_t *reply = g_clock_tx_buffer;
  uint32_t seq = 0;
  uint32_t clock_gen = 0;
  peer::AnchorDomain last_kind = peer::AnchorDomain::NONE;
  uint64_t last_master = 0;
  bool last_valid = false;

  for (;;) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int length = recvfrom(fd, buffer, peer::MAX_DATAGRAM, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
    const int64_t t2_local_ns = local_now_ns();
    if (length <= 0 || from.sin_addr.s_addr != g_config.peer_ip) {
      continue;
    }
    peer::Reader reader(buffer, (size_t) length);
    peer::Header header{};
    peer::Ping ping{};
    if (!peer::read_header(reader, &header) || header.type != (uint8_t) peer::MsgType::PING ||
        !peer::decode_ping(reader, &ping)) {
      continue;
    }

    int64_t offset_ns = 0;
    peer::AnchorDomain kind = peer::AnchorDomain::NONE;
    const bool valid = audio_receiver_peer_network_offset_ns(&offset_ns, &kind);
    const uint64_t master = kind == peer::AnchorDomain::PTP ? ptp_clock_get_master_clock_id() : 0;
    if (valid != last_valid || kind != last_kind || master != last_master) {
      clock_gen++;
      last_valid = valid;
      last_kind = kind;
      last_master = master;
    }

    peer::Pong pong{};
    pong.t1_local_ns = ping.t1_local_ns;
    pong.t2_network_ns = t2_local_ns + offset_ns;
    pong.clock_gen = clock_gen;
    pong.network_valid = valid;
    pong.t3_network_ns = local_now_ns() + offset_ns;
    const size_t reply_length = peer::encode_pong(reply, peer::MAX_DATAGRAM, g_boot_id, seq++, pong);
    (void) sendto(fd, reply, reply_length, 0, reinterpret_cast<sockaddr *>(&from), from_len);
    g_primary_stats.pings++;
  }
}

static esp_err_t start_primary() {
  g_tx_buffer = static_cast<uint8_t *>(airplay_alloc(peer::MAX_DATAGRAM, false));
  g_clock_rx_buffer = static_cast<uint8_t *>(airplay_alloc(peer::MAX_DATAGRAM, false));
  g_clock_tx_buffer = static_cast<uint8_t *>(airplay_alloc(peer::MAX_DATAGRAM, false));
  if (g_tx_buffer == nullptr || g_clock_rx_buffer == nullptr || g_clock_tx_buffer == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  g_ring = static_cast<RingSlot *>(airplay_calloc(RING_SLOTS, sizeof(RingSlot), false));
  const size_t slot_values = (size_t) AUDIO_PCM_BLOCK_MAX_SAMPLES * forward_channels();
  int16_t *pcm = static_cast<int16_t *>(airplay_alloc(RING_SLOTS * slot_values * sizeof(int16_t), false));
  if (g_ring == nullptr || pcm == nullptr) {
    ESP_LOGE(TAG, "Peer ring allocation failed (%u bytes)",
             (unsigned) (RING_SLOTS * slot_values * sizeof(int16_t)));
    airplay_free(g_ring);
    airplay_free(pcm);
    g_ring = nullptr;
    return ESP_ERR_NO_MEM;
  }
  for (uint32_t i = 0; i < RING_SLOTS; i++) {
    g_ring[i].pcm = &pcm[i * slot_values];
  }
  if (xTaskCreatePinnedToCore(primary_task, "peer_tx", DATA_TASK_STACK, nullptr, DATA_TASK_PRIORITY,
                              &g_primary_task, PEER_CORE) != pdPASS ||
      xTaskCreatePinnedToCore(primary_clock_task, "peer_clk", CLOCK_TASK_STACK, nullptr, CLOCK_TASK_PRIORITY,
                              nullptr, PEER_CORE) != pdPASS) {
    ESP_LOGE(TAG, "Peer task creation failed");
    return ESP_ERR_NO_MEM;
  }
  // Last: the taps start copying once the role reads PRIMARY.
  g_role.store(PeerRole::PRIMARY);
  return ESP_OK;
}

// ===========================================================================
// Secondary
// ===========================================================================

static portMUX_TYPE g_clock_lock = portMUX_INITIALIZER_UNLOCKED;
static peer::PeerClock::Line g_relay_line{};
static std::atomic<ClockSource> g_source{ClockSource::NONE};
// The primary's address, from config or learned from its datagrams.
static std::atomic<uint32_t> g_primary_ip{0};
// Only the secondary clock task touches it.
static peer::PeerClock *g_relay_clock = nullptr;

struct RelayStats {
  int64_t last_rtt_ns;
  int64_t min_rtt_ns;
  double rate;
  uint32_t reseeds;
  uint32_t pongs;
};
static RelayStats g_relay_stats{};

bool audio_peer_secondary_offset_ns(int64_t local_ns, int64_t *offset_ns) {
  switch (g_source.load(std::memory_order_relaxed)) {
    case ClockSource::PTP:
      if (!ptp_clock_is_locked()) {
        return false;
      }
      *offset_ns = ptp_clock_get_offset_ns();
      return true;
    case ClockSource::RELAY: {
      portENTER_CRITICAL(&g_clock_lock);
      const peer::PeerClock::Line line = g_relay_line;
      portEXIT_CRITICAL(&g_clock_lock);
      if (!line.valid) {
        return false;
      }
      *offset_ns = peer::PeerClock::offset_on(line, local_ns);
      return true;
    }
    default:
      return false;
  }
}

static bool relay_locked() {
  portENTER_CRITICAL(&g_clock_lock);
  const bool valid = g_relay_line.valid;
  portEXIT_CRITICAL(&g_clock_lock);
  return valid;
}

static void secondary_clock_task(void *arg) {
  (void) arg;
  const int fd = open_udp(g_config.port + 1, PING_INTERVAL_MS);
  if (fd < 0) {
    vTaskDelete(nullptr);
    return;
  }
  peer::PeerClock &clock = *g_relay_clock;
  uint8_t *buffer = g_clock_rx_buffer;
  uint32_t seq = 0;
  uint32_t boot_id = 0;
  uint32_t clock_gen = 0;
  bool have_gen = false;

  for (;;) {
    const uint32_t primary_ip = g_primary_ip.load();
    if (primary_ip == 0) {
      vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
      continue;
    }
    const sockaddr_in to = peer_addr(primary_ip, g_config.port + 1);
    const int64_t t1 = local_now_ns();
    peer::Ping ping{t1};
    const size_t length = peer::encode_ping(buffer, peer::MAX_DATAGRAM, g_boot_id, seq++, ping);
    (void) sendto(fd, buffer, length, 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));

    const int64_t deadline_ns = t1 + (int64_t) PING_INTERVAL_MS * 1000000LL;
    for (;;) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      const int received =
          recvfrom(fd, buffer, peer::MAX_DATAGRAM, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
      const int64_t t4 = local_now_ns();
      if (received > 0 && from.sin_addr.s_addr == primary_ip) {
        peer::Reader reader(buffer, (size_t) received);
        peer::Header header{};
        peer::Pong pong{};
        if (peer::read_header(reader, &header) && header.type == (uint8_t) peer::MsgType::PONG &&
            peer::decode_pong(reader, &pong) && pong.t1_local_ns == t1) {
          // A different primary boot or clock is a different line.
          if (!have_gen || header.boot_id != boot_id || pong.clock_gen != clock_gen || !pong.network_valid) {
            clock.reset();
            boot_id = header.boot_id;
            clock_gen = pong.clock_gen;
            have_gen = true;
          }
          if (pong.network_valid) {
            clock.add_sample(t1, pong.t2_network_ns, pong.t3_network_ns, t4);
          }
          const peer::PeerClock::Line line = clock.line();
          portENTER_CRITICAL(&g_clock_lock);
          g_relay_line = line;
          g_relay_stats.last_rtt_ns = clock.last_rtt_ns();
          g_relay_stats.min_rtt_ns = clock.min_rtt_ns();
          g_relay_stats.rate = clock.rate();
          g_relay_stats.reseeds = clock.reseeds();
          g_relay_stats.pongs++;
          portEXIT_CRITICAL(&g_clock_lock);
          break;
        }
      }
      if (t4 >= deadline_ns) {
        break;
      }
    }
    const int64_t remaining_ns = deadline_ns - local_now_ns();
    if (remaining_ns > 0) {
      vTaskDelay(pdMS_TO_TICKS((uint32_t) (remaining_ns / 1000000LL)) + 1);
    }
  }
}

// What the secondary has applied to its engine, against what the primary
// last said.
struct Mirror {
  bool have_boot;
  uint32_t boot_id;
  bool prepared;
  uint32_t sample_rate;
  uint32_t frame_samples;
  bool epoch_known;
  uint32_t epoch;
  uint32_t flush_serial;
  uint32_t flush_rtp;
  bool have_state;
  peer::State state;
  bool playing;
  bool output_running;
  int32_t volume_q15;
  bool anchor_applied;
  uint32_t applied_rtp;
  uint64_t applied_network_ns;
  int64_t applied_offset_ns;
  bool link_up;
  int64_t last_rx_us;
  bool have_seq;
  uint32_t next_seq;
};
static Mirror g_mirror{};

struct SecondaryStats {
  uint32_t datagrams;
  uint32_t seq_lost;
  uint32_t frames;
  uint32_t incomplete;
  uint32_t stale;
  uint32_t push_fail;
  uint32_t not_ready;
};
static SecondaryStats g_secondary_stats{};

// Reassembly of one forwarded frame from its chunks, which are sent in order.
struct Assembly {
  bool active;
  uint32_t epoch;
  uint32_t rtp;
  uint16_t frame_samples;
  uint8_t channels;
  uint16_t received;
};
static Assembly g_assembly{};
static int16_t *g_frame = nullptr;  // stereo, AUDIO_PCM_BLOCK_MAX_SAMPLES frames
static constexpr size_t CHUNK_VALUES = peer::MAX_PCM_PAYLOAD_BYTES / sizeof(int16_t);
static int16_t *g_chunk = nullptr;
static uint8_t *g_rx_buffer = nullptr;

static void reset_for_boot(uint32_t boot_id) {
  g_mirror.have_boot = true;
  g_mirror.boot_id = boot_id;
  g_mirror.epoch_known = false;
  g_mirror.have_state = false;
  g_mirror.have_seq = false;
  g_assembly.active = false;
}

/// Follow the primary's epoch. False for a datagram from an older one.
static bool follow_epoch(uint32_t epoch, uint32_t flush_serial, uint32_t flush_rtp) {
  if (g_mirror.epoch_known && epoch == g_mirror.epoch) {
    return true;
  }
  if (g_mirror.epoch_known && !peer::serial_newer(epoch, g_mirror.epoch)) {
    return false;
  }
  audio_receiver_peer_begin_epoch();
  audio_output_flush();
  g_mirror.epoch_known = true;
  g_mirror.epoch = epoch;
  // Flushes before this epoch are moot: its timeline starts empty.
  g_mirror.flush_serial = flush_serial;
  g_mirror.flush_rtp = flush_rtp;
  g_mirror.anchor_applied = false;
  g_assembly.active = false;
  return true;
}

static void follow_flush(uint32_t flush_serial, uint32_t flush_rtp) {
  if (!peer::serial_newer(flush_serial, g_mirror.flush_serial)) {
    return;
  }
  audio_receiver_peer_deferred_flush(flush_rtp);
  g_mirror.flush_serial = flush_serial;
  g_mirror.flush_rtp = flush_rtp;
}

static bool ptp_follows(const peer::State &state) {
  return state.domain == peer::AnchorDomain::PTP && state.clock_id != 0 &&
         ptp_clock_get_master_clock_id() == state.clock_id && ptp_clock_is_locked();
}

static ClockSource choose_source(const peer::State &state) {
  const bool ptp_ok = ptp_follows(state);
  const bool relay_ok = relay_locked();
  switch (g_config.clock) {
    case PeerClockMode::PTP:
      return ptp_ok ? ClockSource::PTP : ClockSource::NONE;
    case PeerClockMode::RELAY:
      return relay_ok ? ClockSource::RELAY : ClockSource::NONE;
    default:
      break;
  }
  // Stay on the source in use while it holds: switching steps the offset by
  // the two sources' disagreement.
  const ClockSource current = g_source.load();
  if (current == ClockSource::PTP && ptp_ok) {
    return ClockSource::PTP;
  }
  if (current == ClockSource::RELAY && relay_ok) {
    return ClockSource::RELAY;
  }
  if (ptp_ok) {
    return ClockSource::PTP;
  }
  return relay_ok ? ClockSource::RELAY : ClockSource::NONE;
}

// Publish the primary's clock map once there is a clock to read it against,
// and withdraw it when the clock or the link goes.
static void reconcile_anchor() {
  const peer::State &state = g_mirror.state;
  if (g_mirror.have_state && state.domain == peer::AnchorDomain::PTP && state.clock_id != 0 &&
      ptp_clock_get_master_clock_id() != state.clock_id) {
    ptp_clock_set_master_clock_id(state.clock_id);
  }
  const ClockSource source = g_mirror.have_state ? choose_source(state) : ClockSource::NONE;
  const ClockSource previous = g_source.exchange(source);
  if (source != previous) {
    ESP_LOGI(TAG, "peer clock: %s -> %s (anchor %s)", clock_source_name(previous), clock_source_name(source),
             anchor_domain_name(state.domain));
  }

  const bool want = g_mirror.prepared && g_mirror.epoch_known && g_mirror.link_up && state.anchor_valid &&
                    source != ClockSource::NONE;
  if (!want) {
    if (g_mirror.anchor_applied) {
      audio_receiver_peer_set_anchor(false, 0, 0, 0);
      g_mirror.anchor_applied = false;
    }
    return;
  }
  const int64_t offset_ns = state.playout_offset_ns + (int64_t) g_config.delay_us * 1000LL;
  if (g_mirror.anchor_applied && g_mirror.applied_rtp == state.anchor_rtp &&
      g_mirror.applied_network_ns == state.anchor_network_ns && g_mirror.applied_offset_ns == offset_ns) {
    return;
  }
  audio_receiver_peer_set_anchor(true, state.anchor_rtp, state.anchor_network_ns, offset_ns);
  g_mirror.anchor_applied = true;
  g_mirror.applied_rtp = state.anchor_rtp;
  g_mirror.applied_network_ns = state.anchor_network_ns;
  g_mirror.applied_offset_ns = offset_ns;
}

static void handle_state(const peer::State &state) {
  g_mirror.state = state;
  g_mirror.have_state = true;
  if (!state.engine_ready) {
    // The primary has not streamed since it booted; nothing to mirror yet.
    return;
  }
  bool reset = false;
  if (!g_mirror.prepared || g_mirror.sample_rate != state.sample_rate ||
      g_mirror.frame_samples != state.frame_samples) {
    g_mirror.prepared = audio_receiver_peer_prepare(state.sample_rate, state.frame_samples, &reset);
    g_mirror.sample_rate = state.sample_rate;
    g_mirror.frame_samples = state.frame_samples;
    if (reset) {
      g_mirror.epoch_known = false;
    }
  }
  if (!g_mirror.prepared || !follow_epoch(state.epoch, state.flush_serial, state.flush_rtp)) {
    return;
  }
  follow_flush(state.flush_serial, state.flush_rtp);

  if (state.playing != g_mirror.playing) {
    audio_receiver_peer_set_playing(state.playing);
    if (!state.playing) {
      audio_output_flush();
    }
    g_mirror.playing = state.playing;
  }
  if (state.volume_q15 != g_mirror.volume_q15) {
    audio_output_set_volume_q15(state.volume_q15);
    g_mirror.volume_q15 = state.volume_q15;
  }
  if (state.output_running != g_mirror.output_running) {
    if (state.output_running) {
      audio_output_start();
    } else {
      audio_output_stop();
    }
    g_mirror.output_running = state.output_running;
  }
}

static void handle_pcm(const peer::PcmChunk &chunk, const int16_t *pcm) {
  if (!g_mirror.prepared || chunk.frame_samples > g_mirror.frame_samples) {
    g_secondary_stats.not_ready++;
    return;
  }
  if (!follow_epoch(chunk.epoch, chunk.flush_serial, chunk.flush_rtp)) {
    g_secondary_stats.stale++;
    return;
  }
  // A flush whose STATE was lost is applied before the audio that replaces
  // what it dropped; audio from before it, past its boundary, is dropped.
  if (peer::serial_newer(chunk.flush_serial, g_mirror.flush_serial)) {
    follow_flush(chunk.flush_serial, chunk.flush_rtp);
  } else if (chunk.flush_serial != g_mirror.flush_serial &&
             (int32_t) (chunk.rtp - g_mirror.flush_rtp) >= 0) {
    g_secondary_stats.stale++;
    return;
  }

  if (chunk.offset == 0) {
    if (g_assembly.active) {
      g_secondary_stats.incomplete++;
    }
    g_assembly = Assembly{true, chunk.epoch, chunk.rtp, chunk.frame_samples, chunk.channels, 0};
  } else if (!g_assembly.active || g_assembly.epoch != chunk.epoch || g_assembly.rtp != chunk.rtp ||
             g_assembly.frame_samples != chunk.frame_samples || g_assembly.channels != chunk.channels ||
             g_assembly.received != chunk.offset) {
    if (g_assembly.active) {
      g_secondary_stats.incomplete++;
    }
    g_assembly.active = false;
    return;
  }

  int16_t *out = &g_frame[(size_t) chunk.offset * 2];
  for (size_t i = 0; i < chunk.count; i++) {
    if (chunk.channels == 2) {
      out[2 * i] = pcm[2 * i];
      out[2 * i + 1] = pcm[2 * i + 1];
    } else {
      out[2 * i] = pcm[i];
      out[2 * i + 1] = pcm[i];
    }
  }
  g_assembly.received += chunk.count;
  if (g_assembly.received < g_assembly.frame_samples) {
    return;
  }
  g_assembly.active = false;
  if (audio_receiver_peer_push(chunk.rtp, g_frame, g_assembly.frame_samples, PUSH_TIMEOUT_MS)) {
    g_secondary_stats.frames++;
  } else {
    g_secondary_stats.push_fail++;
  }
}

static void log_secondary_stats() {
  const SecondaryStats stats = g_secondary_stats;
  g_secondary_stats = SecondaryStats{};
  portENTER_CRITICAL(&g_clock_lock);
  const RelayStats relay = g_relay_stats;
  const bool relay_valid = g_relay_line.valid;
  g_relay_stats.pongs = 0;
  portEXIT_CRITICAL(&g_clock_lock);
  if (stats.datagrams == 0 && !g_mirror.link_up) {
    return;
  }
  ESP_LOGI(TAG,
           "peer rx: datagrams=%" PRIu32 " lost=%" PRIu32 " frames=%" PRIu32 " incomplete=%" PRIu32
           " stale=%" PRIu32 " push_fail=%" PRIu32 " not_ready=%" PRIu32
           " link=%d clock=%s anchor=%s/%d ptp=%d relay=%d rtt=%.2f/%.2fms rate=%+.2fppm reseeds=%" PRIu32
           " pongs=%" PRIu32,
           stats.datagrams, stats.seq_lost, stats.frames, stats.incomplete, stats.stale, stats.push_fail,
           stats.not_ready, g_mirror.link_up, clock_source_name(g_source.load()),
           anchor_domain_name(g_mirror.state.domain), g_mirror.anchor_applied, ptp_follows(g_mirror.state),
           relay_valid, (double) relay.last_rtt_ns / 1e6, (double) relay.min_rtt_ns / 1e6, relay.rate * 1e6,
           relay.reseeds, relay.pongs);
}

static void secondary_task(void *arg) {
  (void) arg;
  const int fd = open_udp(g_config.port, RX_TIMEOUT_MS);
  if (fd < 0) {
    vTaskDelete(nullptr);
    return;
  }
  uint8_t *buffer = g_rx_buffer;
  int64_t last_stats_us = esp_timer_get_time();

  for (;;) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int length = recvfrom(fd, buffer, peer::MAX_DATAGRAM, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
    const int64_t now_us = esp_timer_get_time();

    if (length > 0 && (g_config.peer_ip == 0 || from.sin_addr.s_addr == g_config.peer_ip)) {
      peer::Reader reader(buffer, (size_t) length);
      peer::Header header{};
      if (peer::read_header(reader, &header)) {
        if (!g_mirror.have_boot || header.boot_id != g_mirror.boot_id) {
          char ip[INET_ADDRSTRLEN];
          inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
          ESP_LOGI(TAG, "peer: primary %s (boot %08" PRIx32 ")", ip, header.boot_id);
          reset_for_boot(header.boot_id);
          g_primary_ip.store(from.sin_addr.s_addr);
        }
        if (g_mirror.have_seq && header.seq != g_mirror.next_seq) {
          g_secondary_stats.seq_lost += header.seq - g_mirror.next_seq;
        }
        g_mirror.have_seq = true;
        g_mirror.next_seq = header.seq + 1;
        g_secondary_stats.datagrams++;
        g_mirror.last_rx_us = now_us;
        if (!g_mirror.link_up) {
          ESP_LOGI(TAG, "peer: link up");
        }
        g_mirror.link_up = true;

        if (header.type == (uint8_t) peer::MsgType::STATE) {
          peer::State state{};
          if (peer::decode_state(reader, &state)) {
            handle_state(state);
          }
        } else if (header.type == (uint8_t) peer::MsgType::PCM) {
          peer::PcmChunk chunk{};
          if (peer::decode_pcm(reader, &chunk, g_chunk, CHUNK_VALUES)) {
            handle_pcm(chunk, g_chunk);
          }
        }
      }
    }

    if (g_mirror.link_up && now_us - g_mirror.last_rx_us > LINK_TIMEOUT_US) {
      ESP_LOGW(TAG, "peer: link lost, nothing from the primary for %lld ms: muting",
               (long long) ((now_us - g_mirror.last_rx_us) / 1000));
      g_mirror.link_up = false;
      g_assembly.active = false;
    }
    reconcile_anchor();

    if (now_us - last_stats_us >= STATS_INTERVAL_US) {
      log_secondary_stats();
      last_stats_us = now_us;
    }
  }
}

static esp_err_t start_secondary() {
  g_frame = static_cast<int16_t *>(
      airplay_alloc((size_t) AUDIO_PCM_BLOCK_MAX_SAMPLES * 2 * sizeof(int16_t), false));
  g_chunk = static_cast<int16_t *>(airplay_alloc(CHUNK_VALUES * sizeof(int16_t), false));
  g_rx_buffer = static_cast<uint8_t *>(airplay_alloc(peer::MAX_DATAGRAM, false));
  g_clock_rx_buffer = static_cast<uint8_t *>(airplay_alloc(peer::MAX_DATAGRAM, false));
  void *clock_storage = airplay_alloc(sizeof(peer::PeerClock), false);
  if (g_frame == nullptr || g_chunk == nullptr || g_rx_buffer == nullptr || g_clock_rx_buffer == nullptr ||
      clock_storage == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  g_relay_clock = new (clock_storage) peer::PeerClock();
  g_mirror.volume_q15 = -1;
  g_primary_ip.store(g_config.peer_ip);
  if (xTaskCreatePinnedToCore(secondary_task, "peer_rx", DATA_TASK_STACK, nullptr, DATA_TASK_PRIORITY, nullptr,
                              PEER_CORE) != pdPASS ||
      xTaskCreatePinnedToCore(secondary_clock_task, "peer_clk", CLOCK_TASK_STACK, nullptr, CLOCK_TASK_PRIORITY,
                              nullptr, PEER_CORE) != pdPASS) {
    ESP_LOGE(TAG, "Peer task creation failed");
    return ESP_ERR_NO_MEM;
  }
  g_role.store(PeerRole::SECONDARY);
  return ESP_OK;
}

// ===========================================================================

void audio_peer_configure(const PeerConfig &config) {
  if (g_role.load() == PeerRole::STANDALONE) {
    g_config = config;
  }
}

esp_err_t audio_peer_start(void) {
  static bool started = false;
  if (started) {
    return ESP_ERR_INVALID_STATE;
  }
  if (g_config.role == PeerRole::STANDALONE) {
    started = true;
    return ESP_OK;
  }
  g_boot_id = esp_random();
  const esp_err_t err = g_config.role == PeerRole::PRIMARY ? start_primary() : start_secondary();
  if (err == ESP_OK) {
    started = true;
    char ip[INET_ADDRSTRLEN] = "any";
    if (g_config.peer_ip != 0) {
      in_addr addr{};
      addr.s_addr = g_config.peer_ip;
      inet_ntoa_r(addr, ip, sizeof(ip));
    }
    ESP_LOGI(TAG, "peer: %s, peer %s, ports %u/%u, boot %08" PRIx32,
             g_config.role == PeerRole::PRIMARY ? "primary" : "secondary", ip, (unsigned) g_config.port,
             (unsigned) (g_config.port + 1), g_boot_id);
  }
  return err;
}

PeerRole audio_peer_role(void) { return g_role.load(std::memory_order_relaxed); }

void audio_peer_get_status(audio_peer_status_t *status) {
  status->role = g_role.load();
  status->link_up = g_mirror.link_up;
  status->playing = g_mirror.playing;
  status->output_running = g_mirror.output_running;
  status->clock = clock_source_name(g_source.load());
}

}  // namespace airplay_receiver
}  // namespace esphome
