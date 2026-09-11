// airplay_receiver PTP clock — port of upstream main/network/ptp_clock.c.
//
// Passive IEEE 1588 PTP slave for AirPlay 2 time sync.  Listens for PTP
// multicast SYNC / FOLLOW_UP messages on 224.0.1.129:319/320 and maintains a
// filtered offset between the local clock and the PTP master so the audio
// timing engine can map RTP timestamps to wall-clock network time.
//
// PORT NOTES:
//   * Converted to C++ in the esphome::airplay_receiver namespace.
//   * The upstream spiram_task_* wrappers were dropped: they were thin
//     pass-throughs to xTaskCreate in this version of the source, and the
//     component convention (see transport_module.cpp) is to call xTaskCreate
//     directly.  The FreeRTOS task stack is RTOS-managed memory, not
//     component-owned heap.
//   * This module owns no component heap: all timing state is a file-static
//     struct and the socket receive buffer is stack-local, so there is nothing
//     to route through airplay_*.  If a future change introduces dynamic
//     buffers, allocate them via ../allocator.h.
//   * ESP logs go through esphome/core/log.h (TAG = "airplay_ptp").
//   * No dependency on the sibling ntp_clock module — PTP is the AirPlay 2
//     master clock reference; ntp_clock is the AirPlay 1 path only.

#include "ptp_clock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>

#include "esphome/core/log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_ptp";

// PTP multicast addresses and ports
#define PTP_MULTICAST_ADDR "224.0.1.129"
#define PTP_EVENT_PORT     319
#define PTP_GENERAL_PORT   320

// PTP message types
#define PTP_MSG_SYNC       0x0
#define PTP_MSG_DELAY_REQ  0x1
#define PTP_MSG_FOLLOW_UP  0x8
#define PTP_MSG_DELAY_RESP 0x9
#define PTP_MSG_ANNOUNCE   0xB

// PTP header size and timestamp offset
#define PTP_HEADER_SIZE      34
#define PTP_TIMESTAMP_OFFSET 34
#define PTP_TIMESTAMP_SIZE   10

// Synchronization parameters
//
// WiFi-side timestamping jitter on ESP32 is ~20–30 ms in practice, so a tight
// 40 ms lock threshold can take 10+ seconds to satisfy.  Loosen the lock
// criteria to converge in <1 s while still rejecting genuine outliers via
// the median filter:
//   • LOCK_THRESHOLD_NS:    50 ms  — accept normal WiFi jitter
//   • OUTLIER_THRESHOLD_NS: 75 ms  — keep the threshold strictly larger than
//                                   LOCK_THRESHOLD_NS so a borderline sample
//                                   isn't both kept and counted against lock
//   • MIN_SAMPLES_FOR_LOCK: 4      — ~500 ms at 8 Hz SYNC rate
//   • LOCK_STABLE_TIME_MS:  250    — confirm stability without long wait
#define LOCK_THRESHOLD_NS    50000000LL // 50ms - tolerant of WiFi jitter
#define MIN_SAMPLES_FOR_LOCK 4
#define LOCK_STABLE_TIME_MS  250 // 250ms of stable readings to declare lock
#define LOCK_TIMEOUT_MS      5000
#define OUTLIER_THRESHOLD_NS 50000000LL // 50ms - reject samples beyond this
// Asymmetric filter parameters (modeled after nqptp):
// Network delays only ADD positive bias to the measured offset, so
//   offset_measured = true_offset - one_way_delay
// The LARGEST measured offsets correspond to the SHORTEST delays and are
// therefore the most accurate.  We accept positive jitter (= shorter delay)
// quickly and dampen negative jitter (= longer delay) heavily.  This causes
// the filter to converge to the minimum-delay offset, matching the behaviour
// of nqptp used by shairport-sync and ensuring tight multi-room sync.
#define SMOOTH_POS_STARTUP_DIV 1   // accept positive jitter fully at start
#define SMOOTH_POS_STEADY_DIV  16  // later, apply 1/16 of positive jitter
#define SMOOTH_NEG_DIV         256 // always apply only 1/256 of negative jitter
#define SMOOTH_NEG_CLAMP_NS    (-2500000LL) // clamp negative jitter at -2.5ms
#define STARTUP_DURATION_MS 1000 // first second: aggressive positive tracking

// Threshold above which we reset the PTP smoothing filter on resume.
// E.g. at 50 ppm crystal accuracy, 30 s of pause accumulates ~1.5 ms of drift —
// large enough to be audible in multi-room but well within the 50 ms outlier
// window.  Below this threshold the drift is negligible (<0.25 ms at 5 s).
#define PTP_LONG_PAUSE_THRESHOLD_MS 30000

// FreeRTOS task stack depth (bytes) for the PTP listener task.
#define PTP_TASK_STACK_SIZE 4096
#define PTP_TASK_PRIORITY   6

// Receive-path recovery.
//
// The sockets and their IP_ADD_MEMBERSHIP join are created once, at
// ptp_clock_init(), and ensure_ptp_started() is a no-op for every later
// session ("PTP clock already running").  So if multicast forwarding for
// 224.0.1.129 stops reaching this board -- the group pruned by an IGMP
// snooping switch while no master was sending, or an AP roam losing the
// membership -- nothing ever re-establishes it and the task selects() on dead
// sockets forever.  That failure is silent end to end: RTSP, metadata and the
// audio RTP path all stay healthy, but ptp_clock_is_locked() never goes true,
// so audio_receiver_arm_engine_v2_anchor() never publishes an anchor, the
// clock map stays invalid, and audio_scheduler_render() emits silence on every
// call.  Sender connected, metadata scrolling, no sound, no error.
//
// Rebuilding both sockets re-issues the join.  It is cheap (two socket/bind/
// setsockopt triples, no allocation) and idempotent on the wire, so the
// timeout is chosen for how long a person waits for audio, not for how
// expensive the retry is.  A live stream carries ~8 SYNC/s, so a healthy
// session never comes near this; only genuine deafness reaches it.
#define PTP_RX_SILENCE_TIMEOUT_MS 30000

// Packets ARE arriving but every SYNC/FOLLOW_UP is being dropped by the master
// filter (see ptp_clock_set_master_clock_id).  Rebuilding sockets cannot help
// that -- it is a clock_id mismatch, not a dead join -- so it gets a distinct,
// rate-limited report instead of a recovery attempt.
#define PTP_FILTER_DEAF_REPORT_MS 30000

// Unlocked-state counter dump.  Unlocked means no anchor, hence no audio, and
// the counters that say why are printed nowhere else -- but this must stay
// visible at the INFO level the boards actually run (ESPHome refuses a per-tag
// level more verbose than the global one, so a DEBUG-only line cannot be turned
// on without making every other tag chatty too).  It is therefore budgeted
// rather than rate-limited alone: each session start grants
// PTP_UNLOCKED_STATUS_BUDGET reports, and nothing else refills it.  A session
// that locks normally spends none of it; a session that never locks leaves
// ~30 s of evidence and then goes quiet, so an idle board with no master on the
// wire never accumulates log.
//
// The refill MUST hang off the session, not off pinning a master: tying it to
// ptp_clock_set_master_clock_id() meant a session whose sender reused the
// previous clock_id hit that function's early return, granted no budget, and
// produced no diagnostics at all -- silently, and only in the repeat-offender
// case that most needed them.
#define PTP_UNLOCKED_STATUS_MS     5000
#define PTP_UNLOCKED_STATUS_BUDGET 6

// How long a ptp_clock_notify_playing_unlocked() call keeps reporting alive.
// The audio path renews it at render rate (~125/s), so this only has to
// outlive a render loop that has stopped -- i.e. the stream ending, at which
// point reporting must lapse back to the budget so an idle board goes quiet.
#define PTP_STALL_NOTICE_TTL_MS 2000

// PTP state
namespace {
struct PtpState {
  bool running = false;
  TaskHandle_t task_handle = nullptr;
  int event_socket = -1;
  int general_socket = -1;

  // Synchronization state
  bool locked = false;
  uint32_t lock_start_ms = 0;
  uint32_t lock_candidate_start_ms = 0;
  uint32_t last_sync_ms = 0;
  int64_t filtered_offset_ns = 0;  // PTP_time = local_time + offset
  uint32_t sample_count = 0;

  // Asymmetric smoothing state (replaces median ring buffer)
  int64_t previous_offset = 0;
  // Most recent RAW (unsmoothed) offset sample.  The smoothing filter is
  // deliberately asymmetric (see SMOOTH_* below), so filtered_offset_ns can
  // sit a long way from the truth without any existing log revealing it —
  // every timing figure the firmware prints is derived from the filtered
  // value, so an error in it is invisible to those figures.  Keeping the raw
  // sample lets callers compare the two and detect filter divergence.
  int64_t raw_offset_ns = 0;
  uint32_t previous_offset_time_ms = 0;  // 0 = no previous sample yet
  uint32_t mastership_start_ms = 0;      // when continuous tracking began

  // Two-step sync tracking
  uint16_t last_sync_seq = 0;
  int64_t last_sync_local_ns = 0;
  bool awaiting_followup = false;

  // Statistics
  uint32_t sync_count = 0;
  uint32_t followup_count = 0;
  uint32_t announce_count = 0;
  uint32_t rejected_master_count = 0;  // SYNC/FOLLOW_UP from a non-matching master
  uint32_t outlier_count = 0;          // samples rejected by 50ms threshold

  // Master clock filter (0 = accept any master)
  uint64_t expected_clock_id = 0;

  // Receive-path health, tracked PER SOCKET and updated before the master
  // filter runs, so it separates "nothing is reaching us" (rebuild) from
  // "everything is being rejected" (a clock_id mismatch, which a rebuild would
  // not fix).  0 = not seeded yet.
  //
  // The split is load-bearing, not tidiness: SYNC arrives only on the event
  // port and FOLLOW_UP/ANNOUNCE only on the general port, so a single
  // timestamp lets a live general socket keep the watchdog fed while the event
  // socket is deaf -- no SYNC, no samples, no lock, no rejections, and no
  // rebuild either.  That silence survives everything except a reboot.
  uint32_t last_event_packet_ms = 0;
  uint32_t last_general_packet_ms = 0;
  uint32_t socket_rebuilds = 0;
  // Rebuilds this port has not yet received anything after, counted per port
  // and only for the port that was actually silent when the rebuild ran.  A
  // single shared counter made an ANNOUNCE on a healthy general port announce
  // recovery of a still-deaf event socket, which it did twice during the
  // 2026-09-09 incident while the board stayed silent.
  uint32_t event_rebuilds_since_rx = 0;
  uint32_t general_rebuilds_since_rx = 0;
  uint32_t last_filter_report_ms = 0;
  uint32_t rejected_at_last_report = 0;
  uint32_t last_status_report_ms = 0;
  uint32_t status_budget = 0;
  // Written by the audio task, read by ptp_task. A lone 32-bit store, so a
  // torn read is impossible; the worst a race costs is one report.
  volatile uint32_t stall_notice_ms = 0;
};
PtpState ptp{};
}  // namespace

// Cross-task publication of the filtered clock offset. ptp_task WRITES it;
// the playback task (audio_receiver_read -> audio_receiver_network_offset_ns)
// and the RTSP task (audio_timing_set_anchor) READ it. On Xtensa an int64 is
// two 32-bit words, so a plain write/read is a torn read (spurious ±4.29 s
// clock step). Publish through a 32-bit seqlock so every load/store is a
// single instruction (lock-free, IRAM-safe, no libatomic).
static volatile uint32_t g_ptp_offset_seq = 0U;  // even == stable, odd == writing
static volatile uint32_t g_ptp_offset_lo = 0U;
static volatile uint32_t g_ptp_offset_hi = 0U;

static void ptp_publish_offset(int64_t off) {
  const uint32_t s = g_ptp_offset_seq;
  g_ptp_offset_seq = s + 1U;  // odd: a reader knows a write is in progress
  const uint64_t u = (uint64_t) off;
  g_ptp_offset_lo = (uint32_t) u;
  g_ptp_offset_hi = (uint32_t) (u >> 32);
  g_ptp_offset_seq = s + 2U;  // even: stable again
}

static int64_t ptp_read_offset(void) {
  uint32_t s0, s1, lo, hi;
  do {
    s0 = g_ptp_offset_seq;
    lo = g_ptp_offset_lo;
    hi = g_ptp_offset_hi;
    s1 = g_ptp_offset_seq;
  } while (s0 != s1 || (s0 & 1U) != 0U);
  return (int64_t) (((uint64_t) hi << 32) | (uint64_t) lo);
}

// Parse 8-byte clockIdentity (big-endian) from PTP sourcePortIdentity
// (header bytes 20-27).
static uint64_t parse_ptp_clock_id(const uint8_t *data) {
  uint64_t id = 0;
  for (int i = 0; i < 8; i++) {
    id = (id << 8) | data[20 + i];
  }
  return id;
}

// Parse 48-bit seconds + 32-bit nanoseconds from PTP timestamp
static uint64_t parse_ptp_timestamp_ns(const uint8_t *data) {
  // Seconds: 6 bytes big-endian
  uint64_t seconds = 0;
  for (int i = 0; i < 6; i++) {
    seconds = (seconds << 8) | data[i];
  }

  // Nanoseconds: 4 bytes big-endian
  uint32_t nanos = ((uint32_t)data[6] << 24) | ((uint32_t)data[7] << 16) |
                   ((uint32_t)data[8] << 8) | (uint32_t)data[9];

  return seconds * 1000000000ULL + nanos;
}

// Get local time in nanoseconds (from esp_timer)
static inline int64_t get_local_time_ns(void) {
  return (int64_t)esp_timer_get_time() * 1000LL;
}

// Update offset with new sample using asymmetric smoothing (nqptp-style).
//
// The key insight from nqptp: since we are a passive PTP listener (no
// DELAY_REQ/DELAY_RESP), every measured offset contains a one-way network
// delay bias:  offset_measured = true_offset - delay.
// LARGER offsets come from SHORTER delays and are MORE accurate.
//
// By accepting positive jitter (larger offset = shorter delay) quickly and
// dampening negative jitter (smaller offset = longer delay) slowly, the
// filter converges to the offset corresponding to the minimum network
// delay — the best available approximation of the true clock offset.
static void update_offset(int64_t new_offset_ns) {
  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  ptp.last_sync_ms = now_ms;
  ptp.sample_count++;
  // Record the raw sample before any smoothing or outlier rejection, so the
  // divergence between measured and filtered offset stays observable.
  ptp.raw_offset_ns = new_offset_ns;

  int64_t smoothed_offset;

  if (ptp.previous_offset_time_ms == 0) {
    // First sample (or after reset): accept unconditionally
    smoothed_offset = new_offset_ns;
    ptp.mastership_start_ms = now_ms;
  } else {
    // Reject obvious outliers (more than 50ms from current estimate)
    int64_t diff = new_offset_ns - ptp.filtered_offset_ns;
    if (diff < 0) {
      diff = -diff;
    }
    if (diff > OUTLIER_THRESHOLD_NS) {
      ptp.outlier_count++;
      return;
    }

    int64_t jitter = new_offset_ns - ptp.previous_offset;
    uint32_t mastership_time_ms = now_ms - ptp.mastership_start_ms;

    if (jitter >= 0) {
      // Positive jitter: offset increased → shorter network delay → more
      // accurate.  Accept quickly, especially during startup.
      if (mastership_time_ms < STARTUP_DURATION_MS) {
        smoothed_offset = ptp.previous_offset + jitter / SMOOTH_POS_STARTUP_DIV;
      } else {
        smoothed_offset = ptp.previous_offset + jitter / SMOOTH_POS_STEADY_DIV;
      }
    } else {
      // Negative jitter: offset decreased → longer network delay → less
      // reliable.  Clamp and apply only a tiny fraction.
      int64_t clamped_jitter = jitter;
      if (clamped_jitter < SMOOTH_NEG_CLAMP_NS) {
        clamped_jitter = SMOOTH_NEG_CLAMP_NS;
      }
      smoothed_offset = ptp.previous_offset + clamped_jitter / SMOOTH_NEG_DIV;
    }
  }

  ptp.previous_offset = smoothed_offset;
  ptp.previous_offset_time_ms = now_ms;
  ptp.filtered_offset_ns = smoothed_offset;
  ptp_publish_offset(smoothed_offset);  // publish for cross-task (playback/RTSP) readers

  // Check lock status: once we have enough samples and the offset is stable,
  // declare lock.  Use the deviation between the raw sample and the smoothed
  // value as a stability indicator.
  if (ptp.sample_count >= MIN_SAMPLES_FOR_LOCK) {
    int64_t dev = new_offset_ns - smoothed_offset;
    if (dev < 0) {
      dev = -dev;
    }

    if (dev < LOCK_THRESHOLD_NS) {
      if (!ptp.locked) {
        if (ptp.lock_candidate_start_ms == 0) {
          ptp.lock_candidate_start_ms = now_ms;
        }
        if ((now_ms - ptp.lock_candidate_start_ms) >= LOCK_STABLE_TIME_MS) {
          ptp.locked = true;
          ptp.lock_start_ms = now_ms;
          ptp.lock_candidate_start_ms = 0;
          ESP_LOGI(TAG,
                   "LOCKED: offset=%+lldns dev=%lldns samples=%lu "
                   "sync=%lu followup=%lu",
                   (long long)ptp.filtered_offset_ns, (long long)dev,
                   (unsigned long)ptp.sample_count,
                   (unsigned long)ptp.sync_count,
                   (unsigned long)ptp.followup_count);
        }
      }
    } else {
      ptp.lock_candidate_start_ms = 0;
      if (ptp.locked && dev > LOCK_THRESHOLD_NS * 4) {
        ptp.locked = false;
        ptp.lock_start_ms = 0;
        ESP_LOGW(TAG, "LOST LOCK: dev=%lldns (threshold=%lldns)",
                 (long long)dev, (long long)(LOCK_THRESHOLD_NS * 4));
      }
    }
  }
}

// Process SYNC message (records receive time)
static void process_sync(const uint8_t *data, size_t len, uint16_t seq) {
  ptp.sync_count++;
  ptp.last_sync_seq = seq;
  ptp.last_sync_local_ns = get_local_time_ns();
  ptp.awaiting_followup = true;

  // Check if this is a one-step sync (timestamp in SYNC itself)
  // One-step: flags bit 9 (twoStepFlag) is 0
  uint16_t flags = ((uint16_t)data[6] << 8) | data[7];
  bool two_step = (flags & 0x0200) != 0;

  if (!two_step && len >= PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) {
    // One-step sync - timestamp is in the SYNC message
    uint64_t ptp_time_ns = parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);
    // Apply correctionField for one-step sync as well
    if (len >= 16) {
      int64_t correction_field =
          ((int64_t)data[8] << 56) | ((int64_t)data[9] << 48) |
          ((int64_t)data[10] << 40) | ((int64_t)data[11] << 32) |
          ((int64_t)data[12] << 24) | ((int64_t)data[13] << 16) |
          ((int64_t)data[14] << 8) | (int64_t)data[15];
      correction_field /= 65536;  // convert from 2^-16 ns to ns
      ptp_time_ns = (uint64_t)((int64_t)ptp_time_ns + correction_field);
    }
    int64_t offset = (int64_t)ptp_time_ns - ptp.last_sync_local_ns;
    update_offset(offset);
    ptp.awaiting_followup = false;
  }
}

// Process FOLLOW_UP message (contains precise timestamp for preceding SYNC)
static void process_followup(const uint8_t *data, size_t len, uint16_t seq) {
  if (!ptp.awaiting_followup) {
    return;
  }

  // FOLLOW_UP should match the sequence of the last SYNC
  if (seq != ptp.last_sync_seq) {
    return;
  }

  ptp.followup_count++;
  ptp.awaiting_followup = false;

  if (len >= PTP_HEADER_SIZE + PTP_TIMESTAMP_SIZE) {
    uint64_t ptp_time_ns = parse_ptp_timestamp_ns(data + PTP_TIMESTAMP_OFFSET);

    // Apply correctionField (IEEE 1588 §11.4.4.2.1):
    // The correctionField accumulates residence time and path delay
    // corrections from PTP-aware network elements.  It is a signed
    // 64-bit value in units of 2^-16 nanoseconds.
    if (len >= 16) {
      int64_t correction_field =
          ((int64_t)data[8] << 56) | ((int64_t)data[9] << 48) |
          ((int64_t)data[10] << 40) | ((int64_t)data[11] << 32) |
          ((int64_t)data[12] << 24) | ((int64_t)data[13] << 16) |
          ((int64_t)data[14] << 8) | (int64_t)data[15];
      correction_field /= 65536;  // convert from 2^-16 ns to ns
      ptp_time_ns = (uint64_t)((int64_t)ptp_time_ns + correction_field);
    }

    // offset = PTP_time - local_time_at_sync_receipt
    int64_t offset = (int64_t)ptp_time_ns - ptp.last_sync_local_ns;
    update_offset(offset);
  }
}

// Process received PTP message
static void process_ptp_message(const uint8_t *data, size_t len,
                                bool is_event_port) {
  if (len < PTP_HEADER_SIZE) {
    return;
  }

  uint8_t msg_type = data[0] & 0x0F;
  uint16_t seq = ((uint16_t)data[30] << 8) | data[31];

  // If a master filter is set, reject messages from other clocks.
  // This applies only to messages that contribute to offset estimation
  // (SYNC / FOLLOW_UP); ANNOUNCE and others are ignored anyway.
  if (ptp.expected_clock_id != 0 &&
      (msg_type == PTP_MSG_SYNC || msg_type == PTP_MSG_FOLLOW_UP)) {
    uint64_t src_clock_id = parse_ptp_clock_id(data);
    if (src_clock_id != ptp.expected_clock_id) {
      ptp.rejected_master_count++;
      return;
    }
  }

  switch (msg_type) {
  case PTP_MSG_SYNC:
    if (is_event_port) {
      process_sync(data, len, seq);
    }
    break;

  case PTP_MSG_FOLLOW_UP:
    if (!is_event_port) {
      process_followup(data, len, seq);
    }
    break;

  case PTP_MSG_ANNOUNCE:
    ptp.announce_count++;
    // Could track master identity here if needed
    break;

  default:
    break;
  }
}

// Create and bind multicast socket
static int create_ptp_socket(uint16_t port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    return -1;
  }

  // Allow address reuse
  int opt = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // Bind to port
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind to port %d: %d", port, errno);
    close(sock);
    return -1;
  }

  // Join multicast group
  struct ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = inet_addr(PTP_MULTICAST_ADDR);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);

  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    ESP_LOGE(TAG, "Failed to join multicast group: %d", errno);
    close(sock);
    return -1;
  }

  // Set receive timeout
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  return sock;
}

// Tear both sockets down and build them again, which re-issues the multicast
// join.  Called only from ptp_task, so there is no race with the select() that
// uses these descriptors.  A partial failure leaves the failed side at -1; the
// task's `max_fd < 0` branch then idles at 100 ms and the next timeout retries,
// so a transient failure (no IP yet, for instance) costs a delay rather than a
// permanently dead clock.
static void rebuild_ptp_sockets(void) {
  if (ptp.event_socket >= 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
  }
  if (ptp.general_socket >= 0) {
    close(ptp.general_socket);
    ptp.general_socket = -1;
  }

  ptp.event_socket = create_ptp_socket(PTP_EVENT_PORT);
  ptp.general_socket = create_ptp_socket(PTP_GENERAL_PORT);
  ptp.socket_rebuilds++;
}

// Record any datagram, before filtering.  Receiving something proves the join
// is live on THAT port, so this is also where a completed recovery is
// reported: logging it here (rather than at each rebuild) keeps an idle board
// -- no sender, hence no master, hence no traffic -- from emitting a line
// every 30 s forever, while still leaving evidence when a rebuild actually
// fixed something.
//
// The port split is the whole point.  SYNC arrives only on the event port and
// FOLLOW_UP/ANNOUNCE only on the general port, so recovery of one says nothing
// about the other; claiming it did turned a still-deaf event socket into a
// "recovered" line every 30 s.
static void note_ptp_packet(uint32_t now_ms, bool is_event_port) {
  uint32_t &rebuilds_since_rx =
      is_event_port ? ptp.event_rebuilds_since_rx : ptp.general_rebuilds_since_rx;
  if (rebuilds_since_rx > 0) {
    ESP_LOGI(TAG, "PTP %s port receiving again after %lu socket rebuild(s)",
             is_event_port ? "event" : "general",
             (unsigned long) rebuilds_since_rx);
    rebuilds_since_rx = 0;
  }
  if (is_event_port) {
    ptp.last_event_packet_ms = now_ms;
  } else {
    ptp.last_general_packet_ms = now_ms;
  }
}

// Split the two ways the receive path goes deaf, because they need different
// fixes and only one of them is repairable from here.
static void check_ptp_rx_health(uint32_t now_ms) {
  if (ptp.last_event_packet_ms == 0 || ptp.last_general_packet_ms == 0) {
    // Seed both on the first pass through the loop.
    ptp.last_event_packet_ms = now_ms;
    ptp.last_general_packet_ms = now_ms;
    return;
  }

  const uint32_t quiet_event_ms = now_ms - ptp.last_event_packet_ms;
  const uint32_t quiet_general_ms = now_ms - ptp.last_general_packet_ms;

  // Reporting is allowed either from the session-start budget or for as long
  // as the audio path keeps saying it is trying to play without a clock. The
  // budget alone went dark ~30 s into a wedge that lasted minutes; the notice
  // alone would say nothing about a session that never reached playback.
  const uint32_t stall_notice_ms = ptp.stall_notice_ms;
  const bool playing_unlocked =
      stall_notice_ms != 0 &&
      (now_ms - stall_notice_ms) < PTP_STALL_NOTICE_TTL_MS;
  const bool may_report = ptp.status_budget > 0 || playing_unlocked;

  if (!ptp.locked && may_report &&
      (ptp.last_status_report_ms == 0 ||
       (now_ms - ptp.last_status_report_ms) >= PTP_UNLOCKED_STATUS_MS)) {
    ptp.last_status_report_ms = now_ms;
    if (ptp.status_budget > 0) {
      ptp.status_budget--;
    }
    // sync=0 announce=0 with a master pinned means the SENDER stopped sending,
    // not that this board went deaf -- the counters are what tell those apart.
    ESP_LOGI(TAG,
             "unlocked: sync=%lu followup=%lu announce=%lu rejected=%lu "
             "samples=%lu quiet_event=%lu ms quiet_general=%lu ms "
             "rebuilds=%lu master=%016llx",
             (unsigned long) ptp.sync_count, (unsigned long) ptp.followup_count,
             (unsigned long) ptp.announce_count,
             (unsigned long) ptp.rejected_master_count,
             (unsigned long) ptp.sample_count, (unsigned long) quiet_event_ms,
             (unsigned long) quiet_general_ms,
             (unsigned long) ptp.socket_rebuilds,
             (unsigned long long) ptp.expected_clock_id);
  }

  // Arriving but unusable: the filter is pinned to a master that is not the
  // one on the wire.  Report it -- rejected_master_count is otherwise never
  // surfaced anywhere -- and leave the sockets alone.
  const uint32_t rejected = ptp.rejected_master_count;
  if (rejected != ptp.rejected_at_last_report && !ptp.locked) {
    if (ptp.last_filter_report_ms == 0 ||
        (now_ms - ptp.last_filter_report_ms) >= PTP_FILTER_DEAF_REPORT_MS) {
      ESP_LOGW(TAG,
               "PTP traffic present but filtered out: rejected=%lu (+%lu) "
               "expected_master=%016llx samples=%lu -- no lock, so no anchor "
               "and no audio",
               (unsigned long) rejected,
               (unsigned long) (rejected - ptp.rejected_at_last_report),
               (unsigned long long) ptp.expected_clock_id,
               (unsigned long) ptp.sample_count);
      ptp.last_filter_report_ms = now_ms;
      ptp.rejected_at_last_report = rejected;
    }
    return;
  }

  // EITHER socket going quiet is enough: a deaf event port means no SYNC and
  // therefore no clock, however healthy the general port looks.
  if (quiet_event_ms < PTP_RX_SILENCE_TIMEOUT_MS &&
      quiet_general_ms < PTP_RX_SILENCE_TIMEOUT_MS) {
    return;
  }

  // Credit the rebuild only to the port that was actually silent, so the
  // recovery report in note_ptp_packet() cannot be raised by traffic on the
  // port that never stopped.
  if (quiet_event_ms >= PTP_RX_SILENCE_TIMEOUT_MS) {
    ptp.event_rebuilds_since_rx++;
  }
  if (quiet_general_ms >= PTP_RX_SILENCE_TIMEOUT_MS) {
    ptp.general_rebuilds_since_rx++;
  }

  // Assume the multicast join is gone on at least one socket; both are rebuilt
  // because they share the group and the cost is trivial.
  // Quiet once the evidence budget is spent, so an idle board does not log a
  // line every 30 s forever.
  if (may_report) {
    ESP_LOGI(TAG,
             "PTP socket quiet (event=%lu ms general=%lu ms), rebuilding "
             "(rebuild #%lu)",
             (unsigned long) quiet_event_ms, (unsigned long) quiet_general_ms,
             (unsigned long) (ptp.socket_rebuilds + 1U));
  } else {
    ESP_LOGD(TAG,
             "PTP socket quiet (event=%lu ms general=%lu ms), rebuilding "
             "(rebuild #%lu)",
             (unsigned long) quiet_event_ms, (unsigned long) quiet_general_ms,
             (unsigned long) (ptp.socket_rebuilds + 1U));
  }
  rebuild_ptp_sockets();
  ptp.last_event_packet_ms = now_ms;
  ptp.last_general_packet_ms = now_ms;
}

// PTP task - listens for messages on both ports
static void ptp_task(void *pvParameters) {
  (void)pvParameters;
  uint8_t buffer[256];

  while (ptp.running) {
    fd_set read_fds;
    FD_ZERO(&read_fds);

    int max_fd = -1;
    if (ptp.event_socket >= 0) {
      FD_SET(ptp.event_socket, &read_fds);
      if (ptp.event_socket > max_fd) {
        max_fd = ptp.event_socket;
      }
    }
    if (ptp.general_socket >= 0) {
      FD_SET(ptp.general_socket, &read_fds);
      if (ptp.general_socket > max_fd) {
        max_fd = ptp.general_socket;
      }
    }

    if (max_fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

    if (ret < 0) {
      if (!ptp.running) {
        break;  // Sockets closed during shutdown
      }
      if (errno != EINTR) {
        ESP_LOGE(TAG, "select error: %d", errno);
      }
      continue;
    }

    const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (ret > 0) {
      // Check event port (SYNC messages)
      if (ptp.event_socket >= 0 && FD_ISSET(ptp.event_socket, &read_fds)) {
        ssize_t len = recv(ptp.event_socket, buffer, sizeof(buffer), 0);
        if (len > 0) {
          note_ptp_packet(now_ms, true);
          process_ptp_message(buffer, (size_t)len, true);
        }
      }

      // Check general port (FOLLOW_UP messages)
      if (ptp.general_socket >= 0 && FD_ISSET(ptp.general_socket, &read_fds)) {
        ssize_t len = recv(ptp.general_socket, buffer, sizeof(buffer), 0);
        if (len > 0) {
          note_ptp_packet(now_ms, false);
          process_ptp_message(buffer, (size_t)len, false);
        }
      }
    }

    // Runs on both the timeout and the data path: a master that is sending
    // only rejected packets keeps select() busy, so the deaf-filter case would
    // never be reached from the timeout branch alone.
    check_ptp_rx_health(now_ms);
  }

  // Cleanup
  if (ptp.event_socket >= 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
  }
  if (ptp.general_socket >= 0) {
    close(ptp.general_socket);
    ptp.general_socket = -1;
  }

  ptp.task_handle = nullptr;
  vTaskDelete(NULL);
}

esp_err_t ptp_clock_init(void) {
  if (ptp.running) {
    return ESP_ERR_INVALID_STATE;
  }

  ptp = PtpState{};

  // Create sockets
  ptp.event_socket = create_ptp_socket(PTP_EVENT_PORT);
  if (ptp.event_socket < 0) {
    return ESP_FAIL;
  }

  ptp.general_socket = create_ptp_socket(PTP_GENERAL_PORT);
  if (ptp.general_socket < 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
    return ESP_FAIL;
  }

  // Start task
  ptp.running = true;
  BaseType_t ret =
      xTaskCreate(ptp_task, "ptp_clock", PTP_TASK_STACK_SIZE, NULL,
                  PTP_TASK_PRIORITY, &ptp.task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create PTP task");
    close(ptp.event_socket);
    close(ptp.general_socket);
    ptp.event_socket = -1;
    ptp.general_socket = -1;
    ptp.running = false;
    return ESP_FAIL;
  }

  return ESP_OK;
}

void ptp_clock_stop(void) {
  if (!ptp.running) {
    return;
  }

  ptp.running = false;

  // Close sockets to unblock select
  if (ptp.event_socket >= 0) {
    close(ptp.event_socket);
    ptp.event_socket = -1;
  }
  if (ptp.general_socket >= 0) {
    close(ptp.general_socket);
    ptp.general_socket = -1;
  }

  // Wait for task to exit (task sets task_handle = NULL before vTaskDelete)
  for (int i = 0; i < 20 && ptp.task_handle != nullptr; i++) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (ptp.task_handle != nullptr) {
    ESP_LOGW(TAG, "PTP task did not exit in time");
  }
}

void ptp_clock_clear(void) {
  ptp.locked = false;
  ptp.lock_start_ms = 0;
  ptp.lock_candidate_start_ms = 0;
  ptp.last_sync_ms = 0;
  ptp.filtered_offset_ns = 0;
  ptp_publish_offset(0);
  ptp.sample_count = 0;
  ptp.previous_offset = 0;
  ptp.previous_offset_time_ms = 0;
  ptp.mastership_start_ms = 0;

  ptp.last_sync_seq = 0;
  ptp.last_sync_local_ns = 0;
  ptp.awaiting_followup = false;

  ptp.sync_count = 0;
  ptp.followup_count = 0;

  // Drop the master filter so the next session can lock to whatever master
  // its anchor packet names (which may differ from the previous session).
  ptp.expected_clock_id = 0;
}

void ptp_clock_notify_resume(uint32_t pause_duration_ms) {
  if (pause_duration_ms < PTP_LONG_PAUSE_THRESHOLD_MS) {
    return;  // drift too small to matter
  }
  // Reset the "previous sample" pointer so the very next PTP FOLLOW_UP is
  // accepted unconditionally (the first-sample path in update_offset()).
  // This mirrors what nqptp does on its "B" (begin) signal:
  //   "when the clock goes from inactive to active, NQPTP resets clock
  //    smoothing to the new offset" -- nqptp-shm-structures.h
  //
  // We keep filtered_offset_ns so that audio_timing can continue to use the
  // last-known offset until the first new sample arrives (~125 ms away).
  // We also reset mastership_start_ms so the STARTUP_DURATION_MS aggressive-
  // positive window kicks in again for a faster upward catch-up.
  ptp.previous_offset_time_ms = 0;
  ptp.mastership_start_ms = 0;
  ESP_LOGI(TAG,
           "notify_resume: pause=%lu ms, resetting PTP smoothing "
           "(est. drift %.1f ms @ 50ppm)",
           (unsigned long)pause_duration_ms,
           (float)pause_duration_ms * 50.0f / 1000000.0f);
}

bool ptp_clock_is_locked(void) {
  if (ptp.locked && ptp.last_sync_ms > 0) {
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((now_ms - ptp.last_sync_ms) > LOCK_TIMEOUT_MS) {
      ptp.locked = false;
      ptp.lock_start_ms = 0;
      ptp.lock_candidate_start_ms = 0;
    }
  }

  return ptp.locked;
}

uint64_t ptp_clock_get_time_ns(void) {
  int64_t local_ns = get_local_time_ns();
  return (uint64_t)(local_ns + ptp_read_offset());
}

int64_t ptp_clock_get_offset_ns(void) { return ptp_read_offset(); }

void ptp_clock_set_master_clock_id(uint64_t clock_id) {
  if (clock_id == ptp.expected_clock_id) {
    return;
  }

  ESP_LOGI(TAG, "PTP master clock_id %s: %016llx", clock_id ? "set" : "cleared",
           (unsigned long long)clock_id);
  ptp.expected_clock_id = clock_id;
  // The master changed under us, which invalidates every sample we hold; treat
  // it like a fresh start for reporting purposes too.
  ptp_clock_notify_session_start();

  // Drop accumulated samples / lock state — they may have come from a
  // different (wrong) master.
  ptp.locked = false;
  ptp.lock_start_ms = 0;
  ptp.lock_candidate_start_ms = 0;
  ptp.filtered_offset_ns = 0;
  ptp_publish_offset(0);
  ptp.sample_count = 0;
  ptp.previous_offset = 0;
  ptp.previous_offset_time_ms = 0;
  ptp.awaiting_followup = false;
}

uint64_t ptp_clock_get_master_clock_id(void) { return ptp.expected_clock_id; }

void ptp_clock_get_health(ptp_health_t *health) {
  if (health == nullptr) {
    return;
  }

  const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  // Read through ptp_clock_is_locked() rather than ptp.locked so the caller
  // sees the same LOCK_TIMEOUT_MS decay the audio path sees; the flag alone
  // stays true until something asks.
  health->locked = ptp_clock_is_locked();
  health->sync_count = ptp.sync_count;
  health->announce_count = ptp.announce_count;
  health->rejected_master_count = ptp.rejected_master_count;
  health->sample_count = ptp.sample_count;
  health->socket_rebuilds = ptp.socket_rebuilds;
  health->quiet_event_ms = ptp.last_event_packet_ms == 0
                               ? UINT32_MAX
                               : now_ms - ptp.last_event_packet_ms;
  health->quiet_general_ms = ptp.last_general_packet_ms == 0
                                 ? UINT32_MAX
                                 : now_ms - ptp.last_general_packet_ms;
  health->master_clock_id = ptp.expected_clock_id;
}

void ptp_clock_notify_session_start(void) {
  ptp.status_budget = PTP_UNLOCKED_STATUS_BUDGET;
  ptp.last_status_report_ms = 0;
}

void ptp_clock_notify_playing_unlocked(void) {
  // Called from the audio task at render rate; keep it to one store.
  ptp.stall_notice_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void ptp_clock_get_stats(ptp_stats_t *stats) {
  stats->sync_count = ptp.sync_count;
  stats->followup_count = ptp.followup_count;
  // last_offset_ns previously reported previous_offset, which is itself a
  // smoothed value — so it could never reveal filter divergence.  Report the
  // genuinely raw sample instead.
  stats->last_offset_ns = ptp.raw_offset_ns;
  stats->filtered_offset_ns = ptp_read_offset();
  stats->outlier_count = ptp.outlier_count;

  if (ptp.locked && ptp.lock_start_ms > 0) {
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    stats->lock_time_ms = now_ms - ptp.lock_start_ms;
  } else {
    stats->lock_time_ms = 0;
  }
}

}  // namespace airplay_receiver
}  // namespace esphome
