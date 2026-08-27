#pragma once
// airplay_receiver audio_epoch — mono-monotonic epoch/generation counter.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_epoch.{c,h} (PR #130) into
// the ESPHome component. The upstream public API is preserved 1:1 (function
// names, parameter lists and the struct layout are unchanged) and the whole
// module is wrapped in esphome::airplay_receiver so it links cleanly beside the
// other ported slices.
//
// An epoch is a lock-free generation counter used to invalidate stale work
// across a re-cut of the slot pool: every push/take operation binds to the
// epoch it started under, and a re-cut bumps the epoch so in-flight copies
// that crossed the re-cut fail their epoch check rather than corrupting a slot
// that has already been re-handed-out.
//
// MEMORY POLICY: this module owns NO heap — the counter lives in the caller's
// audio_epoch_t storage. There is nothing to route through airplay_alloc/
// airplay_calloc/airplay_free (the ../allocator.h contract holds vacuously),
// and no malloc/calloc/new/free appears anywhere.
//
// All reads/writes are atomic (__atomic_* builtins) so the epoch is safe to
// share between the decode worker and the re-cut path without a lock.

#include <cstdint>

namespace esphome {
namespace airplay_receiver {

typedef struct {
  uint32_t current;
} audio_epoch_t;

/// Reset the epoch to its initial value (1).
void audio_epoch_init(audio_epoch_t *epoch);
/// Read the current epoch value (0 if `epoch` is NULL).
uint32_t audio_epoch_get(const audio_epoch_t *epoch);
/// Atomically advance to the next epoch value and return it (skips the
/// reserved 0 sentinel; 0 if `epoch` is NULL).
uint32_t audio_epoch_advance(audio_epoch_t *epoch);
/// True when `value` is a valid, non-zero epoch that matches the current one.
bool audio_epoch_matches(const audio_epoch_t *epoch, uint32_t value);

}  // namespace airplay_receiver
}  // namespace esphome
