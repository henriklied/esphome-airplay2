#include <inttypes.h>
#include <string.h>
#include <cstdint>
#include <cstddef>

#include "bplist.h"

namespace esphome {
namespace airplay_receiver {
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t b64_decode_table[256] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 62,  255,
    255, 255, 63,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  255, 255,
    255, 0,   255, 255, 255, 0,   1,   2,   3,   4,   5,   6,   7,   8,   9,
    10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
    25,  255, 255, 255, 255, 255, 255, 26,  27,  28,  29,  30,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
    49,  50,  51,  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255};

size_t base64_encoded_length(size_t input_len) {
  return ((input_len + 2) / 3) * 4;
}

int base64_encode(const uint8_t *input, size_t input_len, char *output,
                  size_t output_capacity) {
  if (!input || !output) {
    return -1;
  }

  size_t out_len = base64_encoded_length(input_len);
  if (out_len > output_capacity) {
    return -1;
  }

  size_t pos = 0;
  for (size_t i = 0; i < input_len; i += 3) {
    uint32_t octet_a = i < input_len ? input[i] : 0;
    uint32_t octet_b = i + 1 < input_len ? input[i + 1] : 0;
    uint32_t octet_c = i + 2 < input_len ? input[i + 2] : 0;

    uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

    output[pos++] = b64_table[(triple >> 18) & 0x3F];
    output[pos++] = b64_table[(triple >> 12) & 0x3F];
    output[pos++] =
        (char)((i + 1 < input_len) ? b64_table[(triple >> 6) & 0x3F] : '=');
    output[pos++] =
        (char)((i + 2 < input_len) ? b64_table[triple & 0x3F] : '=');
  }

  return (int)out_len;
}

int base64_decode(const char *input, size_t input_len, uint8_t *output,
                  size_t output_capacity) {
  if (!input || !output || output_capacity == 0) {
    return -1;
  }

  size_t actual_len = 0;
  for (size_t i = 0; i < input_len; i++) {
    char c = input[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      actual_len++;
    }
  }

  size_t output_len = (actual_len * 3) / 4;
  if (output_len > output_capacity) {
    return -1;
  }

  size_t j = 0;
  uint32_t accum = 0;
  int bits = 0;

  for (size_t i = 0; i < input_len; i++) {
    char c = input[i];

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      continue;
    }

    if (c == '=') {
      break;
    }

    uint8_t val = b64_decode_table[(uint8_t)c];
    if (val == 255) {
      return -1;
    }

    accum = (accum << 6) | val;
    bits += 6;

    if (bits >= 8) {
      bits -= 8;
      if (j >= output_capacity) {
        return -1;
      }
      output[j++] = (accum >> bits) & 0xFF;
    }
  }

  return (int)j;
}


static bool bplist_has_room(size_t pos, size_t need, size_t capacity) {
  return pos <= capacity && need <= capacity - pos;
}

static bool bplist_write_u64(uint8_t *out, size_t capacity, size_t *pos,
                             uint64_t value) {
  if (!bplist_has_room(*pos, 8, capacity)) {
    return false;
  }
  for (int i = 7; i >= 0; i--) {
    out[(*pos)++] = (uint8_t)(value >> (i * 8));
  }
  return true;
}

static bool bplist_write_length(uint8_t *out, size_t capacity, size_t *pos,
                                uint8_t marker_base, size_t length) {
  if (length < 15) {
    if (!bplist_has_room(*pos, 1, capacity)) {
      return false;
    }
    out[(*pos)++] = marker_base | (uint8_t)length;
    return true;
  }
  if (length > UINT8_MAX || !bplist_has_room(*pos, 3, capacity)) {
    return false;
  }
  out[(*pos)++] = marker_base | 0x0F;
  out[(*pos)++] = 0x10;
  out[(*pos)++] = (uint8_t)length;
  return true;
}

static bool bplist_write_ascii_string(uint8_t *out, size_t capacity,
                                      size_t *pos, const char *value) {
  size_t len = strlen(value);
  if (!bplist_write_length(out, capacity, pos, 0x50, len) ||
      !bplist_has_room(*pos, len, capacity)) {
    return false;
  }
  memcpy(out + *pos, value, len);
  *pos += len;
  return true;
}

static bool bplist_write_data(uint8_t *out, size_t capacity, size_t *pos,
                              const uint8_t *data, size_t len) {
  if (!bplist_write_length(out, capacity, pos, 0x40, len) ||
      !bplist_has_room(*pos, len, capacity)) {
    return false;
  }
  memcpy(out + *pos, data, len);
  *pos += len;
  return true;
}

static bool bplist_write_int(uint8_t *out, size_t capacity, size_t *pos,
                             uint64_t value) {
  uint8_t marker;
  size_t bytes;
  if (value <= UINT8_MAX) {
    marker = 0x10;
    bytes = 1;
  } else if (value <= UINT16_MAX) {
    marker = 0x11;
    bytes = 2;
  } else if (value <= UINT32_MAX) {
    marker = 0x12;
    bytes = 4;
  } else {
    marker = 0x13;
    bytes = 8;
  }

  if (!bplist_has_room(*pos, 1 + bytes, capacity)) {
    return false;
  }
  out[(*pos)++] = marker;
  for (int i = (int)bytes - 1; i >= 0; i--) {
    out[(*pos)++] = (uint8_t)(value >> (i * 8));
  }
  return true;
}

static bool bplist_write_refs(uint8_t *out, size_t capacity, size_t *pos,
                              const uint8_t *refs, size_t count) {
  if (!bplist_has_room(*pos, count, capacity)) {
    return false;
  }
  memcpy(out + *pos, refs, count);
  *pos += count;
  return true;
}

static bool bplist_write_array(uint8_t *out, size_t capacity, size_t *pos,
                               const uint8_t *refs, size_t count) {
  return bplist_write_length(out, capacity, pos, 0xA0, count) &&
         bplist_write_refs(out, capacity, pos, refs, count);
}

static bool bplist_write_dict(uint8_t *out, size_t capacity, size_t *pos,
                              const uint8_t *keys, const uint8_t *values,
                              size_t count) {
  return bplist_write_length(out, capacity, pos, 0xD0, count) &&
         bplist_write_refs(out, capacity, pos, keys, count) &&
         bplist_write_refs(out, capacity, pos, values, count);
}

static bool bplist_finish(uint8_t *out, size_t capacity, size_t *pos,
                          const size_t *offsets, size_t object_count,
                          size_t top_object) {
  size_t offset_table_offset = *pos;
  for (size_t i = 0; i < object_count; i++) {
    if (offsets[i] > UINT16_MAX || !bplist_has_room(*pos, 2, capacity)) {
      return false;
    }
    out[(*pos)++] = (uint8_t)(offsets[i] >> 8);
    out[(*pos)++] = (uint8_t)offsets[i];
  }

  if (!bplist_has_room(*pos, 32, capacity)) {
    return false;
  }
  memset(out + *pos, 0, 6);
  *pos += 6;
  out[(*pos)++] = 2; // offset size
  out[(*pos)++] = 1; // object ref size

  if (!bplist_write_u64(out, capacity, pos, object_count) ||
      !bplist_write_u64(out, capacity, pos, top_object) ||
      !bplist_write_u64(out, capacity, pos, offset_table_offset)) {
    return false;
  }
  return true;
}

size_t bplist_build_initial_setup(uint8_t *out, size_t capacity,
                                  uint16_t event_port) {
  if (capacity < 100) {
    return 0;
  }

  size_t pos = 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  size_t offsets[10];
  size_t obj = 0;

  offsets[obj++] = pos;
  out[pos++] = 0x59;
  memcpy(out + pos, "eventPort", 9);
  pos += 9;

  offsets[obj++] = pos;
  out[pos++] = 0x5A;
  memcpy(out + pos, "timingPort", 10);
  pos += 10;

  offsets[obj++] = pos;
  out[pos++] = 0x11;
  out[pos++] = (event_port >> 8) & 0xFF;
  out[pos++] = event_port & 0xFF;

  offsets[obj++] = pos;
  out[pos++] = 0x10;
  out[pos++] = 0;

  offsets[obj++] = pos;
  out[pos++] = 0xD2;
  out[pos++] = 0;
  out[pos++] = 1;
  out[pos++] = 2;
  out[pos++] = 3;

  size_t offset_table_offset = pos;
  for (size_t i = 0; i < obj; i++) {
    if (offsets[i] > 0xFF) {
      return 0;
    }
    out[pos++] = (uint8_t)offsets[i];
  }

  memset(out + pos, 0, 6);
  pos += 6;
  out[pos++] = 1;
  out[pos++] = 1;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)obj;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = 4;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)offset_table_offset;

  return pos;
}

size_t bplist_build_stream_setup(uint8_t *out, size_t capacity,
                                 int64_t stream_type, uint16_t data_port,
                                 uint16_t control_port,
                                 uint32_t audio_buffer_size) {
  if (capacity < 200) {
    return 0;
  }

  size_t pos = 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  size_t offsets[16];
  size_t obj = 0;

  offsets[obj++] = pos;
  out[pos++] = 0x57;
  memcpy(out + pos, "streams", 7);
  pos += 7;

  offsets[obj++] = pos;
  out[pos++] = 0x54;
  memcpy(out + pos, "type", 4);
  pos += 4;

  offsets[obj++] = pos;
  out[pos++] = 0x58;
  memcpy(out + pos, "dataPort", 8);
  pos += 8;

  offsets[obj++] = pos;
  out[pos++] = 0x5B;
  memcpy(out + pos, "controlPort", 11);
  pos += 11;

  offsets[obj++] = pos;
  out[pos++] = 0x5F;
  out[pos++] = 0x10;
  out[pos++] = 15;
  memcpy(out + pos, "audioBufferSize", 15);
  pos += 15;

  offsets[obj++] = pos;
  out[pos++] = 0x10;
  out[pos++] = (uint8_t)stream_type;

  offsets[obj++] = pos;
  out[pos++] = 0x11;
  out[pos++] = (data_port >> 8) & 0xFF;
  out[pos++] = data_port & 0xFF;

  offsets[obj++] = pos;
  out[pos++] = 0x11;
  out[pos++] = (control_port >> 8) & 0xFF;
  out[pos++] = control_port & 0xFF;

  offsets[obj++] = pos;
  out[pos++] = 0x12;
  out[pos++] = (audio_buffer_size >> 24) & 0xFF;
  out[pos++] = (audio_buffer_size >> 16) & 0xFF;
  out[pos++] = (audio_buffer_size >> 8) & 0xFF;
  out[pos++] = audio_buffer_size & 0xFF;

  offsets[obj++] = pos;
  if (stream_type == 103) {  // TCP buffered stream
    out[pos++] = 0xD4;
    out[pos++] = 1;
    out[pos++] = 2;
    out[pos++] = 4;
    out[pos++] = 3;
    out[pos++] = 5;
    out[pos++] = 6;
    out[pos++] = 8;
    out[pos++] = 7;
  } else {
    out[pos++] = 0xD3;
    out[pos++] = 1;
    out[pos++] = 2;
    out[pos++] = 3;
    out[pos++] = 5;
    out[pos++] = 6;
    out[pos++] = 7;
  }

  offsets[obj++] = pos;
  out[pos++] = 0xA1;
  out[pos++] = 9;

  offsets[obj++] = pos;
  out[pos++] = 0xD1;
  out[pos++] = 0;
  out[pos++] = 10;

  size_t offset_table_offset = pos;
  for (size_t i = 0; i < obj; i++) {
    if (offsets[i] > 0xFF) {
      return 0;
    }
    out[pos++] = (uint8_t)offsets[i];
  }

  memset(out + pos, 0, 6);
  pos += 6;
  out[pos++] = 1;
  out[pos++] = 1;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)obj;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = 11;

  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)offset_table_offset;

  return pos;
}

size_t bplist_build_feedback_response(uint8_t *out, size_t capacity,
                                      int64_t stream_type, double sample_rate) {
  // Feedback response for buffered audio streams (type 103)
  // Format: { streams: [ { type: 103, sr: 44100.0 } ] }
  // This acts as a keepalive mechanism to prevent iPhone from
  // sending TEARDOWN during extended pause
  if (capacity < 100) {
    return 0;
  }

  size_t pos = 0;
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  size_t offsets[10];
  size_t obj = 0;

  // Object 0: "streams" key string
  offsets[obj++] = pos;
  out[pos++] = 0x57; // String, length 7
  memcpy(out + pos, "streams", 7);
  pos += 7;

  // Object 1: "type" key string
  offsets[obj++] = pos;
  out[pos++] = 0x54; // String, length 4
  memcpy(out + pos, "type", 4);
  pos += 4;

  // Object 2: "sr" key string (sample rate)
  offsets[obj++] = pos;
  out[pos++] = 0x52; // String, length 2
  memcpy(out + pos, "sr", 2);
  pos += 2;

  // Object 3: type value (103 for buffered audio)
  offsets[obj++] = pos;
  out[pos++] = 0x10; // Int, 1 byte
  out[pos++] = (uint8_t)stream_type;

  // Object 4: sample rate value as double
  // IEEE 754 double-precision big-endian
  offsets[obj++] = pos;
  out[pos++] = 0x23; // Real, 8 bytes (double)
  union {
    double d;
    uint8_t bytes[8];
  } sr;
  sr.d = sample_rate;
  // Convert to big-endian
  for (int i = 7; i >= 0; i--) {
    out[pos++] = sr.bytes[i];
  }

  // Object 5: stream dict { type: 3, sr: 4 }
  offsets[obj++] = pos;
  out[pos++] = 0xD2; // Dict, 2 key-value pairs
  out[pos++] = 1;    // Key: object 1 (type)
  out[pos++] = 2;    // Key: object 2 (sr)
  out[pos++] = 3;    // Value: object 3 (type value)
  out[pos++] = 4;    // Value: object 4 (sr value)

  // Object 6: streams array [ object 5 ]
  offsets[obj++] = pos;
  out[pos++] = 0xA1; // Array, 1 element
  out[pos++] = 5;    // Contains object 5 (stream dict)

  // Object 7: top-level dict { streams: 6 }
  offsets[obj++] = pos;
  out[pos++] = 0xD1; // Dict, 1 key-value pair
  out[pos++] = 0;    // Key: object 0 (streams)
  out[pos++] = 6;    // Value: object 6 (streams array)

  // Offset table
  size_t offset_table_offset = pos;
  for (size_t i = 0; i < obj; i++) {
    if (offsets[i] > 0xFF) {
      return 0;
    }
    out[pos++] = (uint8_t)offsets[i];
  }

  // Trailer: 6 unused bytes, then metadata
  memset(out + pos, 0, 6);
  pos += 6;
  out[pos++] = 1; // Offset size
  out[pos++] = 1; // Object ref size

  // Number of objects (8 bytes big-endian)
  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)obj;

  // Top object index (8 bytes big-endian)
  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = 7; // Top object is object 7

  // Offset table offset (8 bytes big-endian)
  for (int i = 0; i < 7; i++) {
    out[pos++] = 0;
  }
  out[pos++] = (uint8_t)offset_table_offset;

  return pos;
}

size_t bplist_build_info_response(uint8_t *out, size_t capacity,
                                  const char *device_id,
                                  const char *device_name,
                                  const uint8_t *public_key,
                                  size_t public_key_len, uint64_t features,
                                  int64_t protocol_version) {
  if (!out || !device_id || !device_name || !public_key ||
      public_key_len == 0 || capacity < 512) {
    return 0;
  }

  size_t pos = 0;
  size_t offsets[39];
  size_t obj = 0;

#define ADD_OFFSET()                                   \
  do {                                                 \
    if (obj >= sizeof(offsets) / sizeof(offsets[0])) { \
      return 0;                                        \
    }                                                  \
    offsets[obj++] = pos;                              \
  } while (0)

  if (!bplist_has_room(pos, 8, capacity)) {
    return 0;
  }
  memcpy(out + pos, "bplist00", 8);
  pos += 8;

  ADD_OFFSET(); // 0: "deviceid"
  if (!bplist_write_ascii_string(out, capacity, &pos, "deviceid")) {
    return 0;
  }
  ADD_OFFSET(); // 1: device id
  if (!bplist_write_ascii_string(out, capacity, &pos, device_id)) {
    return 0;
  }
  ADD_OFFSET(); // 2: "features"
  if (!bplist_write_ascii_string(out, capacity, &pos, "features")) {
    return 0;
  }
  ADD_OFFSET(); // 3: features
  if (!bplist_write_int(out, capacity, &pos, features)) {
    return 0;
  }
  ADD_OFFSET(); // 4: "model"
  if (!bplist_write_ascii_string(out, capacity, &pos, "model")) {
    return 0;
  }
  ADD_OFFSET(); // 5: model
  if (!bplist_write_ascii_string(out, capacity, &pos, "AudioAccessory5,1")) {
    return 0;
  }
  ADD_OFFSET(); // 6: "protovers"
  if (!bplist_write_ascii_string(out, capacity, &pos, "protovers")) {
    return 0;
  }
  ADD_OFFSET(); // 7: protocol version string
  if (!bplist_write_ascii_string(out, capacity, &pos, "1.1")) {
    return 0;
  }
  ADD_OFFSET(); // 8: "srcvers"
  if (!bplist_write_ascii_string(out, capacity, &pos, "srcvers")) {
    return 0;
  }
  ADD_OFFSET(); // 9: source version string
  if (!bplist_write_ascii_string(out, capacity, &pos, "377.40.00")) {
    return 0;
  }
  ADD_OFFSET(); // 10: "vv"
  if (!bplist_write_ascii_string(out, capacity, &pos, "vv")) {
    return 0;
  }
  ADD_OFFSET(); // 11: vv value
  if (!bplist_write_int(out, capacity, &pos, (uint64_t)protocol_version)) {
    return 0;
  }
  ADD_OFFSET(); // 12: "statusFlags"
  if (!bplist_write_ascii_string(out, capacity, &pos, "statusFlags")) {
    return 0;
  }
  ADD_OFFSET(); // 13: statusFlags value
  if (!bplist_write_int(out, capacity, &pos, 4)) {
    return 0;
  }
  ADD_OFFSET(); // 14: "pk"
  if (!bplist_write_ascii_string(out, capacity, &pos, "pk")) {
    return 0;
  }
  ADD_OFFSET(); // 15: public key
  if (!bplist_write_data(out, capacity, &pos, public_key, public_key_len)) {
    return 0;
  }
  ADD_OFFSET(); // 16: "pi"
  if (!bplist_write_ascii_string(out, capacity, &pos, "pi")) {
    return 0;
  }
  ADD_OFFSET(); // 17: pairing identifier
  if (!bplist_write_ascii_string(out, capacity, &pos,
                                 "00000000-0000-0000-0000-000000000000")) {
    return 0;
  }
  ADD_OFFSET(); // 18: "name"
  if (!bplist_write_ascii_string(out, capacity, &pos, "name")) {
    return 0;
  }
  ADD_OFFSET(); // 19: device name
  if (!bplist_write_ascii_string(out, capacity, &pos, device_name)) {
    return 0;
  }
  ADD_OFFSET(); // 20: "audioFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioFormats")) {
    return 0;
  }
  ADD_OFFSET(); // 21: "type"
  if (!bplist_write_ascii_string(out, capacity, &pos, "type")) {
    return 0;
  }
  ADD_OFFSET(); // 22: "audioInputFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioInputFormats")) {
    return 0;
  }
  ADD_OFFSET(); // 23: "audioOutputFormats"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioOutputFormats")) {
    return 0;
  }
  ADD_OFFSET(); // 24: stream type 96
  if (!bplist_write_int(out, capacity, &pos, 96)) {
    return 0;
  }
  ADD_OFFSET(); // 25: format mask
  if (!bplist_write_int(out, capacity, &pos, 0x01000000)) {
    return 0;
  }
  ADD_OFFSET(); // 26: audio format dict
  {
    const uint8_t keys[] = {21, 22, 23};
    const uint8_t values[] = {24, 25, 25};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 3)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 27: audioFormats array
  {
    const uint8_t refs[] = {26};
    if (!bplist_write_array(out, capacity, &pos, refs, 1)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 28: "audioLatencies"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioLatencies")) {
    return 0;
  }
  ADD_OFFSET(); // 29: "audioType"
  if (!bplist_write_ascii_string(out, capacity, &pos, "audioType")) {
    return 0;
  }
  ADD_OFFSET(); // 30: "inputLatencyMicros"
  if (!bplist_write_ascii_string(out, capacity, &pos, "inputLatencyMicros")) {
    return 0;
  }
  ADD_OFFSET(); // 31: "outputLatencyMicros"
  if (!bplist_write_ascii_string(out, capacity, &pos, "outputLatencyMicros")) {
    return 0;
  }
  ADD_OFFSET(); // 32: stream type 103
  if (!bplist_write_int(out, capacity, &pos, 103)) {
    return 0;
  }
  ADD_OFFSET(); // 33: audio type
  if (!bplist_write_int(out, capacity, &pos, 0x64)) {
    return 0;
  }
  ADD_OFFSET(); // 34: zero latency
  if (!bplist_write_int(out, capacity, &pos, 0)) {
    return 0;
  }
  ADD_OFFSET(); // 35: latency dict for realtime stream
  {
    const uint8_t keys[] = {21, 29, 30, 31};
    const uint8_t values[] = {24, 33, 34, 34};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 4)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 36: latency dict for buffered stream
  {
    const uint8_t keys[] = {21, 29, 30, 31};
    const uint8_t values[] = {32, 33, 34, 34};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 4)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 37: audioLatencies array
  {
    const uint8_t refs[] = {35, 36};
    if (!bplist_write_array(out, capacity, &pos, refs, 2)) {
      return 0;
    }
  }
  ADD_OFFSET(); // 38: top-level info dict
  {
    const uint8_t keys[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 28};
    const uint8_t values[] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 27, 37};
    if (!bplist_write_dict(out, capacity, &pos, keys, values, 12)) {
      return 0;
    }
  }

#undef ADD_OFFSET

  if (obj != sizeof(offsets) / sizeof(offsets[0]) ||
      !bplist_finish(out, capacity, &pos, offsets, obj, 38)) {
    return 0;
  }

  return pos;
}


// Binary plist object types (high nibble of marker byte)
#define BPLIST_NULL    0x00
#define BPLIST_BOOL    0x00
#define BPLIST_INT     0x10
#define BPLIST_REAL    0x20
#define BPLIST_DATE    0x30
#define BPLIST_DATA    0x40
#define BPLIST_STRING  0x50
#define BPLIST_UNICODE 0x60
#define BPLIST_UID     0x80
#define BPLIST_ARRAY   0xA0
#define BPLIST_SET     0xC0
#define BPLIST_DICT    0xD0

static uint64_t read_be_int(const uint8_t *data, size_t bytes) {
  uint64_t val = 0;
  for (size_t i = 0; i < bytes; i++) {
    val = (val << 8) | data[i];
  }
  return val;
}

static bool bplist_parse_trailer(const uint8_t *plist, size_t plist_len,
                                 uint8_t *offset_size, uint8_t *ref_size,
                                 uint64_t *num_objects, uint64_t *top_object,
                                 uint64_t *offset_table_offset) {
  if (plist_len < 32) {
    return false;
  }

  const uint8_t *trailer = plist + plist_len - 32;

  *offset_size = trailer[6];
  *ref_size = trailer[7];
  *num_objects = read_be_int(trailer + 8, 8);
  *top_object = read_be_int(trailer + 16, 8);
  *offset_table_offset = read_be_int(trailer + 24, 8);

  return (*offset_size > 0 && *offset_size <= 8 && *ref_size > 0 &&
          *ref_size <= 8 && *offset_table_offset < plist_len);
}

static uint64_t bplist_get_offset(const uint8_t *plist,
                                  uint64_t offset_table_offset,
                                  uint8_t offset_size, uint64_t obj_idx) {
  const uint8_t *entry = plist + offset_table_offset + obj_idx * offset_size;
  return read_be_int(entry, offset_size);
}

static bool bplist_read_string(const uint8_t *plist, size_t plist_len,
                               uint64_t offset, char *out,
                               size_t out_capacity) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    if ((len_marker & 0xF0) != BPLIST_INT) {
      return false;
    }
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    len = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (type == BPLIST_STRING) {
    if (pos + len > plist_len || len >= out_capacity) {
      return false;
    }
    memcpy(out, plist + pos, len);
    out[len] = '\0';
    return true;
  }

  if (type == BPLIST_UNICODE) {
    size_t bytes = len * 2;
    if (pos + bytes > plist_len || len >= out_capacity) {
      return false;
    }
    for (size_t i = 0; i < len; i++) {
      uint16_t code =
          (uint16_t)(plist[pos + i * 2] << 8) | plist[pos + i * 2 + 1];
      if (code > 0x7F) {
        return false;
      }
      out[i] = (char)code;
    }
    out[len] = '\0';
    return true;
  }

  return false;
}

static bool bplist_read_data(const uint8_t *plist, size_t plist_len,
                             uint64_t offset, uint8_t *out, size_t out_capacity,
                             size_t *out_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    if ((len_marker & 0xF0) != BPLIST_INT) {
      return false;
    }
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    len = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (type == BPLIST_DATA) {
    if (pos + len > plist_len || len > out_capacity) {
      return false;
    }
    memcpy(out, plist + pos, len);
    *out_len = len;
    return true;
  }

  return false;
}

static bool bplist_read_data_len(const uint8_t *plist, size_t plist_len,
                                 uint64_t offset, size_t *out_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    if ((len_marker & 0xF0) != BPLIST_INT) {
      return false;
    }
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    len = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (type == BPLIST_DATA) {
    if (pos + len > plist_len) {
      return false;
    }
    *out_len = len;
    return true;
  }

  return false;
}

static bool bplist_read_string_len(const uint8_t *plist, size_t plist_len,
                                   uint64_t offset, size_t *out_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;
  size_t len = marker & 0x0F;
  size_t pos = offset + 1;

  if (len == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    if ((len_marker & 0xF0) != BPLIST_INT) {
      return false;
    }
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    len = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (type == BPLIST_STRING) {
    if (pos + len > plist_len) {
      return false;
    }
    *out_len = len;
    return true;
  }
  if (type == BPLIST_UNICODE) {
    size_t bytes = len * 2;
    if (pos + bytes > plist_len) {
      return false;
    }
    *out_len = len;
    return true;
  }

  return false;
}

static bool bplist_read_int(const uint8_t *plist, size_t plist_len,
                            uint64_t offset, int64_t *out) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;

  if (type == BPLIST_INT) {
    size_t len = 1 << (marker & 0x0F);
    if (offset + 1 + len > plist_len) {
      return false;
    }
    *out = (int64_t)read_be_int(plist + offset + 1, len);
    return true;
  }

  return false;
}

static bool bplist_read_real(const uint8_t *plist, size_t plist_len,
                             uint64_t offset, double *out) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;

  if (type == BPLIST_REAL) {
    size_t len = 1 << (marker & 0x0F);
    if (offset + 1 + len > plist_len) {
      return false;
    }

    if (len == 4) {
      uint32_t bits = (uint32_t)read_be_int(plist + offset + 1, 4);
      float f;
      memcpy(&f, &bits, sizeof(f));
      *out = (double)f;
      return true;
    } else if (len == 8) {
      uint64_t bits = read_be_int(plist + offset + 1, 8);
      memcpy(out, &bits, sizeof(*out));
      return true;
    }
  }

  return false;
}

static bool bplist_parse_count(const uint8_t *plist, size_t plist_len,
                               uint64_t offset, size_t *count,
                               size_t *header_len) {
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  size_t info = marker & 0x0F;
  size_t pos = offset + 1;

  if (info == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    *count = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  } else {
    *count = info;
  }

  *header_len = pos - offset;
  return true;
}

static bool bplist_find_data_in_dict(const uint8_t *plist, size_t plist_len,
                                     uint64_t dict_offset,
                                     uint64_t offset_table_offset,
                                     uint8_t offset_size, uint8_t ref_size,
                                     const char *key, uint8_t *out_data,
                                     size_t out_capacity, size_t *out_len) {
  if (dict_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = 0;
  size_t header_len = 0;
  if (!bplist_parse_count(plist, plist_len, dict_offset, &dict_size,
                          &header_len)) {
    return false;
  }

  size_t pos = dict_offset + header_len;
  if (pos + dict_size * 2 * ref_size > plist_len) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, offset_table_offset, offset_size, val_idx);
        return bplist_read_data(plist, plist_len, val_offset, out_data,
                                out_capacity, out_len);
      }
    }
  }

  return false;
}

static bool bplist_find_data_recursive(const uint8_t *plist, size_t plist_len,
                                       uint64_t obj_idx,
                                       uint64_t offset_table_offset,
                                       uint8_t offset_size, uint8_t ref_size,
                                       const char *key, uint8_t *out_data,
                                       size_t out_capacity, size_t *out_len,
                                       int depth) {
  if (depth > 10) {
    return false;
  }

  uint64_t offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, obj_idx);
  if (offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[offset];
  uint8_t type = marker & 0xF0;

  if (type == BPLIST_DICT) {
    size_t dict_size = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &dict_size,
                            &header_len)) {
      return false;
    }

    size_t pos = offset + header_len;
    if (pos + dict_size * 2 * ref_size > plist_len) {
      return false;
    }

    const uint8_t *key_refs = plist + pos;
    const uint8_t *val_refs = plist + pos + dict_size * ref_size;

    for (size_t i = 0; i < dict_size; i++) {
      uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
      uint64_t key_offset =
          bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

      char found_key[64];
      if (bplist_read_string(plist, plist_len, key_offset, found_key,
                             sizeof(found_key))) {
        if (strcmp(found_key, key) == 0) {
          uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
          uint64_t val_offset = bplist_get_offset(plist, offset_table_offset,
                                                  offset_size, val_idx);
          return bplist_read_data(plist, plist_len, val_offset, out_data,
                                  out_capacity, out_len);
        }
      }
    }

    for (size_t i = 0; i < dict_size; i++) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      if (bplist_find_data_recursive(
              plist, plist_len, val_idx, offset_table_offset, offset_size,
              ref_size, key, out_data, out_capacity, out_len, depth + 1)) {
        return true;
      }
    }
  } else if (type == BPLIST_ARRAY || type == BPLIST_SET) {
    size_t count = 0;
    size_t header_len = 0;
    if (!bplist_parse_count(plist, plist_len, offset, &count, &header_len)) {
      return false;
    }

    size_t pos = offset + header_len;
    if (pos + count * ref_size > plist_len) {
      return false;
    }

    for (size_t i = 0; i < count; i++) {
      uint64_t idx = read_be_int(plist + pos + i * ref_size, ref_size);
      if (bplist_find_data_recursive(plist, plist_len, idx, offset_table_offset,
                                     offset_size, ref_size, key, out_data,
                                     out_capacity, out_len, depth + 1)) {
        return true;
      }
    }
  }

  return false;
}

bool bplist_find_data(const uint8_t *plist, size_t plist_len, const char *key,
                      uint8_t *out_data, size_t out_capacity, size_t *out_len) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  return bplist_find_data_in_dict(plist, plist_len, top_offset,
                                  offset_table_offset, offset_size, ref_size,
                                  key, out_data, out_capacity, out_len);
}

bool bplist_find_data_deep(const uint8_t *plist, size_t plist_len,
                           const char *key, uint8_t *out_data,
                           size_t out_capacity, size_t *out_len) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  (void)num_objects;
  return bplist_find_data_recursive(plist, plist_len, top_object,
                                    offset_table_offset, offset_size, ref_size,
                                    key, out_data, out_capacity, out_len, 0);
}

bool bplist_find_int(const uint8_t *plist, size_t plist_len, const char *key,
                     int64_t *out_value) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[top_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = marker & 0x0F;
  size_t pos = top_offset + 1;

  if (dict_size == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    dict_size = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (pos + dict_size * 2 * ref_size > plist_len) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, offset_table_offset, offset_size, val_idx);
        return bplist_read_int(plist, plist_len, val_offset, out_value);
      }
    }
  }

  return false;
}

bool bplist_find_real(const uint8_t *plist, size_t plist_len, const char *key,
                      double *out_value) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[top_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = marker & 0x0F;
  size_t pos = top_offset + 1;

  if (dict_size == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    dict_size = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (pos + dict_size * 2 * ref_size > plist_len) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, offset_table_offset, offset_size, val_idx);
        if (bplist_read_real(plist, plist_len, val_offset, out_value)) {
          return true;
        }
        int64_t int_val = 0;
        if (bplist_read_int(plist, plist_len, val_offset, &int_val)) {
          *out_value = (double)int_val;
          return true;
        }
        return false;
      }
    }
  }

  return false;
}

bool bplist_find_string(const uint8_t *plist, size_t plist_len, const char *key,
                        char *out_str, size_t out_capacity) {
  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  uint8_t marker = plist[top_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = marker & 0x0F;
  size_t pos = top_offset + 1;

  if (dict_size == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    dict_size = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (pos + dict_size * 2 * ref_size > plist_len) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

    char found_key[64];
    if (bplist_read_string(plist, plist_len, key_offset, found_key,
                           sizeof(found_key))) {
      if (strcmp(found_key, key) == 0) {
        uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
        uint64_t val_offset =
            bplist_get_offset(plist, offset_table_offset, offset_size, val_idx);
        return bplist_read_string(plist, plist_len, val_offset, out_str,
                                  out_capacity);
      }
    }
  }

  return false;
}

bool bplist_get_streams_count(const uint8_t *plist, size_t plist_len,
                              size_t *count) {
  if (!count) {
    return false;
  }

  *count = 0;

  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  size_t streams_key_len = 0;
  uint64_t streams_key_offset = 0;
  for (uint64_t i = 0; i < num_objects; i++) {
    uint64_t offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, i);
    if (offset >= plist_len) {
      continue;
    }
    char key[16];
    if (bplist_read_string(plist, plist_len, offset, key, sizeof(key))) {
      if (strcmp(key, "streams") == 0) {
        streams_key_offset = offset;
        if (!bplist_read_string_len(plist, plist_len, offset,
                                    &streams_key_len)) {
          return false;
        }
        break;
      }
    }
  }

  if (streams_key_len == 0) {
    return false;
  }

  uint64_t top_dict_offset = top_offset;
  uint8_t marker = plist[top_dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = marker & 0x0F;
  size_t pos = top_dict_offset + 1;
  if (dict_size == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    dict_size = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (pos + dict_size * 2 * ref_size > plist_len) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

    if (key_offset == streams_key_offset) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      uint64_t val_offset =
          bplist_get_offset(plist, offset_table_offset, offset_size, val_idx);

      uint8_t val_marker = plist[val_offset];
      if ((val_marker & 0xF0) != BPLIST_ARRAY) {
        return false;
      }

      size_t array_count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, val_offset, &array_count,
                              &header_len)) {
        return false;
      }

      *count = array_count;
      return true;
    }
  }

  return false;
}

bool bplist_get_stream_info(const uint8_t *plist, size_t plist_len,
                            size_t index, int64_t *type, size_t *ekey_len,
                            size_t *eiv_len, size_t *shk_len) {
  if (!type) {
    return false;
  }

  *type = -1;
  if (ekey_len) {
    *ekey_len = 0;
  }
  if (eiv_len) {
    *eiv_len = 0;
  }
  if (shk_len) {
    *shk_len = 0;
  }

  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  size_t streams_key_len = 0;
  uint64_t streams_key_offset = 0;
  for (uint64_t i = 0; i < num_objects; i++) {
    uint64_t offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, i);
    if (offset >= plist_len) {
      continue;
    }
    char key[16];
    if (bplist_read_string(plist, plist_len, offset, key, sizeof(key))) {
      if (strcmp(key, "streams") == 0) {
        streams_key_offset = offset;
        if (!bplist_read_string_len(plist, plist_len, offset,
                                    &streams_key_len)) {
          return false;
        }
        break;
      }
    }
  }

  if (streams_key_len == 0) {
    return false;
  }

  uint64_t top_dict_offset = top_offset;
  uint8_t marker = plist[top_dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = marker & 0x0F;
  size_t pos = top_dict_offset + 1;
  if (dict_size == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    dict_size = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (pos + dict_size * 2 * ref_size > plist_len) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

    if (key_offset == streams_key_offset) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      uint64_t val_offset =
          bplist_get_offset(plist, offset_table_offset, offset_size, val_idx);

      uint8_t val_marker = plist[val_offset];
      if ((val_marker & 0xF0) != BPLIST_ARRAY) {
        return false;
      }

      size_t array_count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, val_offset, &array_count,
                              &header_len)) {
        return false;
      }

      if (index >= array_count) {
        return false;
      }

      size_t array_pos = val_offset + header_len;
      if (array_pos + array_count * ref_size > plist_len) {
        return false;
      }

      uint64_t stream_idx =
          read_be_int(plist + array_pos + index * ref_size, ref_size);
      uint64_t stream_offset = bplist_get_offset(plist, offset_table_offset,
                                                 offset_size, stream_idx);

      uint8_t stream_marker = plist[stream_offset];
      if ((stream_marker & 0xF0) != BPLIST_DICT) {
        return false;
      }

      size_t stream_dict_size = 0;
      size_t stream_header_len = 0;
      if (!bplist_parse_count(plist, plist_len, stream_offset,
                              &stream_dict_size, &stream_header_len)) {
        return false;
      }

      size_t stream_pos = stream_offset + stream_header_len;
      if (stream_pos + stream_dict_size * 2 * ref_size > plist_len) {
        return false;
      }

      const uint8_t *stream_key_refs = plist + stream_pos;
      const uint8_t *stream_val_refs =
          plist + stream_pos + stream_dict_size * ref_size;

      for (size_t j = 0; j < stream_dict_size; j++) {
        uint64_t stream_key_idx =
            read_be_int(stream_key_refs + j * ref_size, ref_size);
        uint64_t stream_key_offset = bplist_get_offset(
            plist, offset_table_offset, offset_size, stream_key_idx);

        char stream_key[32];
        if (!bplist_read_string(plist, plist_len, stream_key_offset, stream_key,
                                sizeof(stream_key))) {
          continue;
        }

        uint64_t stream_val_idx =
            read_be_int(stream_val_refs + j * ref_size, ref_size);
        uint64_t stream_val_offset = bplist_get_offset(
            plist, offset_table_offset, offset_size, stream_val_idx);

        if (strcmp(stream_key, "type") == 0) {
          int64_t type_val = 0;
          if (bplist_read_int(plist, plist_len, stream_val_offset, &type_val)) {
            *type = type_val;
          }
        } else if (strcmp(stream_key, "ekey") == 0 && ekey_len) {
          bplist_read_data_len(plist, plist_len, stream_val_offset, ekey_len);
        } else if (strcmp(stream_key, "eiv") == 0 && eiv_len) {
          bplist_read_data_len(plist, plist_len, stream_val_offset, eiv_len);
        } else if (strcmp(stream_key, "shk") == 0 && shk_len) {
          bplist_read_data_len(plist, plist_len, stream_val_offset, shk_len);
        }
      }

      return (*type != -1);
    }
  }

  return false;
}

bool bplist_get_stream_kv_info(const uint8_t *plist, size_t plist_len,
                               size_t index, bplist_kv_info_t *out,
                               size_t out_capacity, size_t *out_count) {
  if (!out || out_capacity == 0 || !out_count) {
    return false;
  }

  *out_count = 0;

  if (plist_len < 40 || memcmp(plist, "bplist00", 8) != 0) {
    return false;
  }

  uint8_t offset_size = 0;
  uint8_t ref_size = 0;
  uint64_t num_objects = 0;
  uint64_t top_object = 0;
  uint64_t offset_table_offset = 0;
  if (!bplist_parse_trailer(plist, plist_len, &offset_size, &ref_size,
                            &num_objects, &top_object, &offset_table_offset)) {
    return false;
  }

  uint64_t top_offset =
      bplist_get_offset(plist, offset_table_offset, offset_size, top_object);
  if (top_offset >= plist_len) {
    return false;
  }

  size_t streams_key_len = 0;
  uint64_t streams_key_offset = 0;
  for (uint64_t i = 0; i < num_objects; i++) {
    uint64_t offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, i);
    if (offset >= plist_len) {
      continue;
    }
    char key[16];
    if (bplist_read_string(plist, plist_len, offset, key, sizeof(key))) {
      if (strcmp(key, "streams") == 0) {
        streams_key_offset = offset;
        if (!bplist_read_string_len(plist, plist_len, offset,
                                    &streams_key_len)) {
          return false;
        }
        break;
      }
    }
  }

  if (streams_key_len == 0) {
    return false;
  }

  uint64_t top_dict_offset = top_offset;
  uint8_t marker = plist[top_dict_offset];
  if ((marker & 0xF0) != BPLIST_DICT) {
    return false;
  }

  size_t dict_size = marker & 0x0F;
  size_t pos = top_dict_offset + 1;
  if (dict_size == 0x0F) {
    if (pos >= plist_len) {
      return false;
    }
    uint8_t len_marker = plist[pos++];
    size_t len_bytes = 1 << (len_marker & 0x0F);
    if (pos + len_bytes > plist_len) {
      return false;
    }
    dict_size = (size_t)read_be_int(plist + pos, len_bytes);
    pos += len_bytes;
  }

  if (pos + dict_size * 2 * ref_size > plist_len) {
    return false;
  }

  const uint8_t *key_refs = plist + pos;
  const uint8_t *val_refs = plist + pos + dict_size * ref_size;

  for (size_t i = 0; i < dict_size; i++) {
    uint64_t key_idx = read_be_int(key_refs + i * ref_size, ref_size);
    uint64_t key_offset =
        bplist_get_offset(plist, offset_table_offset, offset_size, key_idx);

    if (key_offset == streams_key_offset) {
      uint64_t val_idx = read_be_int(val_refs + i * ref_size, ref_size);
      uint64_t val_offset =
          bplist_get_offset(plist, offset_table_offset, offset_size, val_idx);

      uint8_t val_marker = plist[val_offset];
      if ((val_marker & 0xF0) != BPLIST_ARRAY) {
        return false;
      }

      size_t array_count = 0;
      size_t header_len = 0;
      if (!bplist_parse_count(plist, plist_len, val_offset, &array_count,
                              &header_len)) {
        return false;
      }

      if (index >= array_count) {
        return false;
      }

      size_t array_pos = val_offset + header_len;
      if (array_pos + array_count * ref_size > plist_len) {
        return false;
      }

      uint64_t stream_idx =
          read_be_int(plist + array_pos + index * ref_size, ref_size);
      uint64_t stream_offset = bplist_get_offset(plist, offset_table_offset,
                                                 offset_size, stream_idx);

      uint8_t stream_marker = plist[stream_offset];
      if ((stream_marker & 0xF0) != BPLIST_DICT) {
        return false;
      }

      size_t stream_dict_size = 0;
      size_t stream_header_len = 0;
      if (!bplist_parse_count(plist, plist_len, stream_offset,
                              &stream_dict_size, &stream_header_len)) {
        return false;
      }

      size_t stream_pos = stream_offset + stream_header_len;
      if (stream_pos + stream_dict_size * 2 * ref_size > plist_len) {
        return false;
      }

      const uint8_t *stream_key_refs = plist + stream_pos;
      const uint8_t *stream_val_refs =
          plist + stream_pos + stream_dict_size * ref_size;

      for (size_t j = 0; j < stream_dict_size && *out_count < out_capacity;
           j++) {
        uint64_t stream_key_idx =
            read_be_int(stream_key_refs + j * ref_size, ref_size);
        uint64_t stream_key_offset = bplist_get_offset(
            plist, offset_table_offset, offset_size, stream_key_idx);

        char stream_key[64];
        if (!bplist_read_string(plist, plist_len, stream_key_offset, stream_key,
                                sizeof(stream_key))) {
          continue;
        }

        uint64_t stream_val_idx =
            read_be_int(stream_val_refs + j * ref_size, ref_size);
        uint64_t stream_val_offset = bplist_get_offset(
            plist, offset_table_offset, offset_size, stream_val_idx);

        bplist_kv_info_t *info = &out[*out_count];
        memset(info, 0, sizeof(*info));
        strlcpy(info->key, stream_key, sizeof(info->key));

        uint8_t stream_val_marker = plist[stream_val_offset];
        uint8_t stream_val_type = stream_val_marker & 0xF0;

        if (stream_val_type == BPLIST_INT) {
          info->value_type = BPLIST_VALUE_INT;
          int64_t int_val = 0;
          if (bplist_read_int(plist, plist_len, stream_val_offset, &int_val)) {
            info->int_value = int_val;
          }
        } else if (stream_val_type == BPLIST_DATA) {
          info->value_type = BPLIST_VALUE_DATA;
          size_t len = 0;
          if (bplist_read_data_len(plist, plist_len, stream_val_offset, &len)) {
            info->value_len = len;
          }
        } else if (stream_val_type == BPLIST_STRING ||
                   stream_val_type == BPLIST_UNICODE) {
          info->value_type = BPLIST_VALUE_STRING;
          size_t len = 0;
          if (bplist_read_string_len(plist, plist_len, stream_val_offset,
                                     &len)) {
            info->value_len = len;
          }
        } else if (stream_val_type == BPLIST_UID) {
          info->value_type = BPLIST_VALUE_UID;
        } else if (stream_val_type == BPLIST_ARRAY) {
          info->value_type = BPLIST_VALUE_ARRAY;
        } else if (stream_val_type == BPLIST_DICT) {
          info->value_type = BPLIST_VALUE_DICT;
        }

        (*out_count)++;
      }

      return (*out_count > 0);
    }
  }

  return false;
}

bool bplist_find_stream_crypto(const uint8_t *plist, size_t plist_len,
                               int64_t stream_type, uint8_t *ekey,
                               size_t ekey_capacity, size_t *ekey_len,
                               uint8_t *eiv, size_t eiv_capacity,
                               size_t *eiv_len, uint8_t *shk,
                               size_t shk_capacity, size_t *shk_len) {
  bool found = false;

  if (ekey_len) {
    *ekey_len = 0;
  }
  if (eiv_len) {
    *eiv_len = 0;
  }
  if (shk_len) {
    *shk_len = 0;
  }

  size_t stream_count = 0;
  if (!bplist_get_streams_count(plist, plist_len, &stream_count)) {
    return false;
  }

  for (size_t i = 0; i < stream_count; i++) {
    int64_t type = -1;
    size_t local_ekey_len = 0;
    size_t local_eiv_len = 0;
    size_t local_shk_len = 0;

    if (!bplist_get_stream_info(plist, plist_len, i, &type, &local_ekey_len,
                                &local_eiv_len, &local_shk_len)) {
      continue;
    }

    if (type != stream_type) {
      continue;
    }

    uint8_t temp_buf[512];
    size_t temp_len = 0;

    if (ekey && local_ekey_len > 0 && ekey_len) {
      if (bplist_find_data(plist, plist_len, "ekey", temp_buf, sizeof(temp_buf),
                           &temp_len)) {
        size_t copy_len = temp_len < ekey_capacity ? temp_len : ekey_capacity;
        memcpy(ekey, temp_buf, copy_len);
        *ekey_len = copy_len;
        found = true;
      }
    }

    if (eiv && local_eiv_len > 0 && eiv_len) {
      if (bplist_find_data(plist, plist_len, "eiv", temp_buf, sizeof(temp_buf),
                           &temp_len)) {
        size_t copy_len = temp_len < eiv_capacity ? temp_len : eiv_capacity;
        memcpy(eiv, temp_buf, copy_len);
        *eiv_len = copy_len;
        found = true;
      }
    }

    if (shk && local_shk_len > 0 && shk_len) {
      if (bplist_find_data(plist, plist_len, "shk", temp_buf, sizeof(temp_buf),
                           &temp_len)) {
        size_t copy_len = temp_len < shk_capacity ? temp_len : shk_capacity;
        memcpy(shk, temp_buf, copy_len);
        *shk_len = copy_len;
        found = true;
      }
    }

    break;
  }

  return found;
}

}  // namespace airplay_receiver
}  // namespace esphome