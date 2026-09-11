#pragma once
// airplay_receiver PTP clock — port of upstream main/network/ptp_clock.{c,h}.
//
// Simple PTP (IEEE 1588 RFC 1588-2008 / 802.1AS-style) slave used for AirPlay 2
// time synchronization.  Listens for PTP multicast SYNC / FOLLOW_UP messages
// and tracks the offset between the local clock and the PTP master, producing
// the shared "network time" reference the audio timing engine uses to map RTP
// timestamps to wall-clock time.
//
// This is a PASSIVE listener: it never sends DELAY_REQ or DELAY_RESP, so every
// measured offset carries a (signed) one-way network-delay bias.  The offset
// filter is deliberately asymmetric (nqptp-style) to home in on the
// minimum-delay offset — see the SMOOTH_* constants in ptp_clock.cpp.
//
// Shared port contract: lives in the esphome::airplay_receiver namespace;
// compiled as C++; all state is file-static and the socket receive buffer is
// stack-local, so this module owns NO heap of its own — there is nothing to
// route through airplay_* beyond the RTOS-managed FreeRTOS task stack (created
// with xTaskCreate).  ESP logs go through esphome/core/log.h.  The upstream
// ptp_clock_* public API is preserved exactly.
//
// NOTE ON NTP: this module is fully self-contained and does NOT depend on the
// sibling ntp_clock module (that one is used for AirPlay 1 timing; PTP is the
// AirPlay 2 master clock reference).  It requires only an active PTP master on
// 224.0.1.129:319/320.

#include <cstdint>

#include "esp_err.h"

namespace esphome {
namespace airplay_receiver {

/**
 * Initialize and start PTP clock synchronization.
 * Creates a task that listens for PTP multicast messages.
 */
esp_err_t ptp_clock_init(void);

/**
 * Stop PTP clock and free resources.
 */
void ptp_clock_stop(void);

/**
 * Clear PTP clock synchronization state.
 * Resets offset and lock status without stopping the clock.
 * Called during TEARDOWN to allow re-sync on new session.
 */
void ptp_clock_clear(void);

/**
 * Check if PTP is locked to a master clock.
 * @return true if synchronized with acceptable accuracy
 */
bool ptp_clock_is_locked(void);

/**
 * Get current PTP time in nanoseconds.
 * Returns local time adjusted by PTP offset.
 * @return PTP time in nanoseconds since epoch
 */
uint64_t ptp_clock_get_time_ns(void);

/**
 * Get current offset from local clock to PTP time in nanoseconds.
 * PTP_time = local_time + offset
 */
int64_t ptp_clock_get_offset_ns(void);

/**
 * Notify the PTP clock that playback is resuming after a pause.
 *
 * If the pause lasted longer than PTP_LONG_PAUSE_THRESHOLD_MS, this
 * resets the asymmetric smoothing filter so the next received PTP sample is
 * accepted unconditionally — mirroring the nqptp "B" (begin) signal behaviour
 * described in nqptp-shm-structures.h.
 *
 * Without this, after a pause long enough for the local crystal to drift,
 * the 1/256-negative-jitter damping would take several minutes to re-converge,
 * causing audible multi-room sync loss on resume.
 *
 * @param pause_duration_ms  Wall-clock length of the pause in milliseconds.
 */
void ptp_clock_notify_resume(uint32_t pause_duration_ms);

/**
 * Get synchronization statistics.
 */
typedef struct {
  uint32_t sync_count;        // Number of SYNC messages received
  uint32_t followup_count;    // Number of FOLLOW_UP messages received
  int64_t last_offset_ns;     // Last RAW measured offset (pre-smoothing)
  int64_t filtered_offset_ns; // Filtered/averaged offset (what timing uses)
  uint32_t lock_time_ms;      // Time since lock achieved (0 if not locked)
  uint32_t outlier_count;     // Samples rejected as outliers since start
} ptp_stats_t;

void ptp_clock_get_stats(ptp_stats_t *stats);

/**
 * Restrict the PTP clock to a single master identified by its 8-byte
 * clockIdentity (the value carried in the AirPlay 2 0xD7 anchor packet at
 * offset +20, and in the PTP common-header sourcePortIdentity field at
 * bytes 20-27).
 *
 * Pass 0 to clear the filter (accept any master — the default at startup).
 *
 * When the expected clock_id changes, the filter resets samples and lock
 * state so a stale offset to a previous (possibly wrong) master is not
 * carried over.  On a network with multiple PTP-speaking Apple devices
 * (HomePods, AppleTVs, other receivers), this is what prevents us from
 * locking to the wrong master and computing nonsense early/late deltas.
 */
void ptp_clock_set_master_clock_id(uint64_t clock_id);

/**
 * Read the current expected master clock_id (0 if none / filter cleared).
 */
uint64_t ptp_clock_get_master_clock_id(void);

/**
 * Tell the clock that an AirPlay session is starting, so that if the lock does
 * not follow, the unlocked-state counters get reported.
 *
 * Call this on every session start, including when the PTP task is already
 * running -- that case is precisely the one worth reporting, because nothing
 * else re-arms the diagnostics for a session that reuses an existing clock.
 */
void ptp_clock_notify_session_start(void);

}  // namespace airplay_receiver
}  // namespace esphome
