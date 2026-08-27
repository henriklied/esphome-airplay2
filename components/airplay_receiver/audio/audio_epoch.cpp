// airplay_receiver audio_epoch — mono-monotonic epoch/generation counter.
//
// Port of rbouteiller/airplay-esp32 main/audio/audio_epoch.c (PR #130) into the
// ESPHome component. The upstream logic is preserved exactly; the only change
// is that everything is wrapped in esphome::airplay_receiver. The module owns
// no heap and makes no system calls, so there is no airplay_* allocation and no
// ESP logging — it is pure lock-free counter arithmetic.

#include "audio_epoch.h"

namespace esphome {
namespace airplay_receiver {

void audio_epoch_init(audio_epoch_t *epoch) {
  if (!epoch) {
    return;
  }
  __atomic_store_n(&epoch->current, 1U, __ATOMIC_RELEASE);
}

uint32_t audio_epoch_get(const audio_epoch_t *epoch) {
  if (!epoch) {
    return 0;
  }
  return __atomic_load_n(&epoch->current, __ATOMIC_ACQUIRE);
}

uint32_t audio_epoch_advance(audio_epoch_t *epoch) {
  if (!epoch) {
    return 0;
  }

  uint32_t next = __atomic_add_fetch(&epoch->current, 1U, __ATOMIC_ACQ_REL);
  if (next == 0U) {
    uint32_t expected = 0U;
    (void) __atomic_compare_exchange_n(&epoch->current, &expected, 1U, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    next = audio_epoch_get(epoch);
  }
  return next;
}

bool audio_epoch_matches(const audio_epoch_t *epoch, uint32_t value) {
  return value != 0U && audio_epoch_get(epoch) == value;
}

}  // namespace airplay_receiver
}  // namespace esphome
