#pragma once
// airplay_receiver binary-plist (Apple) parser + builder.
//
// Port of the upstream main/plist/bplist_{parser,builder}.c for the AirPlay 2
// control plane:
//   * builders — /info response, initial SETUP (eventPort/timingPort), stream
//     SETUP (streams[] with dataPort/controlPort/audioBufferSize), feedback
//     keepalive for buffered streams;
//   * parser — read the SETUP/ANNOUNCE/command/feedback/teardown bplist bodies
//     (streams[], ct/sr/spf, ekey/eiv/shk crypto, metadata, rate/anchor).
//
// These operate on caller-provided buffers (no heap), so they need no
// airplay_* routing; callers that hold bplist bodies allocate via airplay_*.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

// ---------------------------------------------------------------------------
// Base64 (port of main/plist/base64.c)
// ---------------------------------------------------------------------------
size_t base64_encoded_length(size_t input_len);
int base64_encode(const uint8_t *input, size_t input_len, char *output, size_t output_capacity);
int base64_decode(const char *input, size_t input_len, uint8_t *output, size_t output_capacity);

// ---------------------------------------------------------------------------
// Binary plist parser
// ---------------------------------------------------------------------------
bool bplist_find_data(const uint8_t *plist, size_t plist_len, const char *key, uint8_t *out_data,
                      size_t out_capacity, size_t *out_len);
bool bplist_find_data_deep(const uint8_t *plist, size_t plist_len, const char *key, uint8_t *out_data,
                           size_t out_capacity, size_t *out_len);
bool bplist_get_streams_count(const uint8_t *plist, size_t plist_len, size_t *count);
bool bplist_get_stream_info(const uint8_t *plist, size_t plist_len, size_t index, int64_t *type,
                            size_t *ekey_len, size_t *eiv_len, size_t *shk_len);

typedef struct {
  char key[64];
  uint8_t value_type;  // BPLIST_VALUE_*
  size_t value_len;
  int64_t int_value;
} bplist_kv_info_t;

#define BPLIST_VALUE_UNKNOWN 0
#define BPLIST_VALUE_INT 1
#define BPLIST_VALUE_DATA 2
#define BPLIST_VALUE_STRING 3
#define BPLIST_VALUE_UID 4
#define BPLIST_VALUE_ARRAY 5
#define BPLIST_VALUE_DICT 6

bool bplist_get_stream_kv_info(const uint8_t *plist, size_t plist_len, size_t index, bplist_kv_info_t *out,
                               size_t out_capacity, size_t *out_count);
bool bplist_find_stream_crypto(const uint8_t *plist, size_t plist_len, int64_t stream_type, uint8_t *ekey,
                               size_t ekey_capacity, size_t *ekey_len, uint8_t *eiv, size_t eiv_capacity,
                               size_t *eiv_len, uint8_t *shk, size_t shk_capacity, size_t *shk_len);
bool bplist_find_int(const uint8_t *plist, size_t plist_len, const char *key, int64_t *out_value);
bool bplist_find_real(const uint8_t *plist, size_t plist_len, const char *key, double *out_value);
bool bplist_find_string(const uint8_t *plist, size_t plist_len, const char *key, char *out_str,
                        size_t out_capacity);

// ---------------------------------------------------------------------------
// Binary plist builders
// ---------------------------------------------------------------------------
size_t bplist_build_initial_setup(uint8_t *out, size_t capacity, uint16_t event_port);
size_t bplist_build_stream_setup(uint8_t *out, size_t capacity, int64_t stream_type, uint16_t data_port,
                                 uint16_t control_port, uint32_t audio_buffer_size);
size_t bplist_build_feedback_response(uint8_t *out, size_t capacity, int64_t stream_type, double sample_rate);
size_t bplist_build_info_response(uint8_t *out, size_t capacity, const char *device_id,
                                  const char *device_name, const uint8_t *public_key, size_t public_key_len,
                                  uint64_t features, int64_t protocol_version);

}  // namespace airplay_receiver
}  // namespace esphome
