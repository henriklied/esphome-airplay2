#include "dsp_spec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace airplay_receiver {

namespace {

constexpr float MIN_FREQUENCY_HZ = 10.0f;
constexpr float MAX_FREQUENCY_HZ = 24000.0f;
constexpr float MIN_Q = 0.05f;
constexpr float MAX_Q = 20.0f;
constexpr float MAX_ABS_GAIN_DB = 24.0f;
constexpr size_t FIELDS_PER_SECTION = 4;

struct TypeCode {
  const char *code;
  airplay_dsp_filter_type_t type;
};

constexpr TypeCode TYPE_CODES[] = {
    {"pk", AIRPLAY_DSP_PEAKING},   {"ls", AIRPLAY_DSP_LOW_SHELF}, {"hs", AIRPLAY_DSP_HIGH_SHELF},
    {"hp", AIRPLAY_DSP_HIGH_PASS}, {"lp", AIRPLAY_DSP_LOW_PASS},  {"no", AIRPLAY_DSP_NOTCH},
};

bool type_from_code(const char *begin, size_t len, airplay_dsp_filter_type_t *out) {
  for (const auto &entry : TYPE_CODES) {
    if (strlen(entry.code) == len && strncmp(entry.code, begin, len) == 0) {
      *out = entry.type;
      return true;
    }
  }
  return false;
}

const char *code_from_type(airplay_dsp_filter_type_t type) {
  for (const auto &entry : TYPE_CODES) {
    if (entry.type == type) {
      return entry.code;
    }
  }
  return nullptr;
}

/// Parse a float spanning exactly [begin, end).
bool parse_float(const char *begin, const char *end, float *out) {
  if (begin == end) {
    return false;
  }
  char buf[24];
  const size_t len = (size_t) (end - begin);
  if (len >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, begin, len);
  buf[len] = '\0';
  char *stop = nullptr;
  const float value = strtof(buf, &stop);
  if (stop != buf + len || !std::isfinite(value)) {
    return false;
  }
  *out = value;
  return true;
}

bool parse_section(const char *begin, const char *end, AirPlayDspFilter *out) {
  const char *fields[FIELDS_PER_SECTION + 1];
  size_t n = 0;
  fields[n++] = begin;
  for (const char *p = begin; p < end; p++) {
    if (*p == ',') {
      if (n == FIELDS_PER_SECTION) {
        return false;
      }
      fields[n++] = p + 1;
    }
  }
  if (n != FIELDS_PER_SECTION) {
    return false;
  }
  fields[FIELDS_PER_SECTION] = end + 1;  // one past the virtual trailing comma
  AirPlayDspFilter filter;
  if (!type_from_code(fields[0], (size_t) (fields[1] - 1 - fields[0]), &filter.type) ||
      !parse_float(fields[1], fields[2] - 1, &filter.frequency_hz) || !parse_float(fields[2], fields[3] - 1, &filter.q) ||
      !parse_float(fields[3], end, &filter.gain_db)) {
    return false;
  }
  if (filter.frequency_hz < MIN_FREQUENCY_HZ || filter.frequency_hz > MAX_FREQUENCY_HZ || filter.q < MIN_Q ||
      filter.q > MAX_Q || std::fabs(filter.gain_db) > MAX_ABS_GAIN_DB) {
    return false;
  }
  *out = filter;
  return true;
}

}  // namespace

bool dsp_spec_parse(const char *spec, AirPlayDspFilter *out, size_t capacity, size_t *count) {
  if (spec == nullptr || out == nullptr || count == nullptr || *spec == '\0') {
    return false;
  }
  if (strcmp(spec, DSP_SPEC_FLAT) == 0) {
    *count = 0;
    return true;
  }
  AirPlayDspFilter parsed[AIRPLAY_DSP_MAX_FILTERS];
  size_t n = 0;
  const char *section = spec;
  while (true) {
    const char *end = strchr(section, ';');
    if (end == nullptr) {
      end = section + strlen(section);
    }
    if (n >= capacity || n >= AIRPLAY_DSP_MAX_FILTERS || !parse_section(section, end, &parsed[n])) {
      return false;
    }
    n++;
    if (*end == '\0') {
      break;
    }
    section = end + 1;
  }
  for (size_t i = 0; i < n; i++) {
    out[i] = parsed[i];
  }
  *count = n;
  return true;
}

size_t dsp_spec_format(const AirPlayDspFilter *filters, size_t count, char *out, size_t capacity) {
  if (out == nullptr || capacity == 0) {
    return 0;
  }
  if (count == 0) {
    const int len = snprintf(out, capacity, "%s", DSP_SPEC_FLAT);
    return (len > 0 && (size_t) len < capacity) ? (size_t) len : 0;
  }
  size_t pos = 0;
  for (size_t i = 0; i < count; i++) {
    const char *code = code_from_type(filters[i].type);
    if (code == nullptr) {
      return 0;
    }
    const int len = snprintf(out + pos, capacity - pos, "%s%s,%g,%.2f,%g", i ? ";" : "", code,
                             (double) filters[i].frequency_hz, (double) filters[i].q, (double) filters[i].gain_db);
    if (len < 0 || (size_t) len >= capacity - pos) {
      out[0] = '\0';
      return 0;
    }
    pos += (size_t) len;
  }
  return pos;
}

}  // namespace airplay_receiver
}  // namespace esphome
