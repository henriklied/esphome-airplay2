#pragma once
// Runs part of FDK's per-channel filterbank on the other core.
//
// The filterbank (IMDCT + overlap-add, CBlock_FrequencyToTime) is ~43% of a
// 5.1 frame and runs per channel after the whole frame is parsed, so channels
// are independent there. Link-time wraps (-Wl,--wrap, set in __init__.py)
// hand every other channel to a worker on core 0 and wait for it before
// CAacDecoder_DecodeFrame returns. Output is bit-identical to the serial path;
// see aac_fdk_parallel.cpp for the conditions that keep it so.
//
// One decoder at a time: the worker and its scratch are process-wide.
// ESP-IDF only; on the host every function is a no-op.

namespace esphome {
namespace airplay_receiver {

/// Start the worker if it is not running. Returns false if it cannot be
/// created, in which case decoding stays serial.
bool aac_fdk_parallel_start();

/// Stop the worker and release its stack and scratch.
void aac_fdk_parallel_stop();

/// Whether the next frames may use the worker (it must be running).
void aac_fdk_parallel_enable(bool enabled);

}  // namespace airplay_receiver
}  // namespace esphome
