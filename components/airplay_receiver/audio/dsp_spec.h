#pragma once
// Text form of the filter cascade, for a Home Assistant `text` entity.
//
// One section per `;`, fields `type,frequency_hz,q,gain_db`:
//
//   hp,80,0.71,0;ls,150,0.70,1.5;pk,200,0.90,-2
//
// Types: pk (peaking), ls (low shelf), hs (high shelf), hp (high-pass),
// lp (low-pass), no (notch). Eight sections fit in HA's 255-character state.
// `flat` is an explicit empty cascade; an empty string means "not set" and is
// the caller's to interpret (the component keeps its YAML filters).
//
// Pure C++ with no ESP-IDF dependency, so the host tests build it as is.
#include <cstddef>

#include "audio_dsp.h"

namespace esphome {
namespace airplay_receiver {

/// The text for an explicitly empty cascade.
constexpr const char *DSP_SPEC_FLAT = "flat";

/**
 * Parse `spec` into `out`. Nothing is written unless the whole spec is valid.
 *
 * @param spec      NUL-terminated text, as above.
 * @param out       Receives up to `capacity` sections.
 * @param capacity  Size of `out`; more sections than this is an error.
 * @param count     Number of sections parsed (0 for `flat`).
 * @return false on an unknown type, a malformed number, a value out of range,
 *         too many sections, or an empty string.
 */
bool dsp_spec_parse(const char *spec, AirPlayDspFilter *out, size_t capacity, size_t *count);

/**
 * Write `filters` as text. Returns the length written (excluding the NUL), or
 * 0 if `capacity` is too small. An empty cascade is written as `flat`.
 */
size_t dsp_spec_format(const AirPlayDspFilter *filters, size_t count, char *out, size_t capacity);

}  // namespace airplay_receiver
}  // namespace esphome
