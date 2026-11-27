#include "group_spec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace airplay_receiver {

namespace {

constexpr const char *STANDALONE_TEXT = "standalone";
/// Same bound as the YAML `delay:` (0-100 ms): a board cannot play earlier than its pipeline.
constexpr int32_t MAX_DELAY_US = 100000;
constexpr long MAX_PORT = 65534;
constexpr int IPV4_OCTETS = 4;
constexpr int MAX_OCTET = 255;

template<typename E> struct Word {
  const char *word;
  E value;
};

constexpr Word<PeerRole> ROLES[] = {
    {"standalone", PeerRole::STANDALONE}, {"primary", PeerRole::PRIMARY}, {"secondary", PeerRole::SECONDARY}};
constexpr Word<PeerForward> CHANNELS[] = {{"left", PeerForward::LEFT},
                                          {"right", PeerForward::RIGHT},
                                          {"mono", PeerForward::MONO},
                                          {"stereo", PeerForward::STEREO}};
constexpr Word<PeerClockMode> CLOCKS[] = {
    {"auto", PeerClockMode::AUTO}, {"ptp", PeerClockMode::PTP}, {"relay", PeerClockMode::RELAY}};

template<typename E, size_t N> bool lookup(const Word<E> (&table)[N], const char *word, E *out) {
  for (const auto &entry : table) {
    if (strcmp(entry.word, word) == 0) {
      *out = entry.value;
      return true;
    }
  }
  return false;
}

template<typename E, size_t N> const char *word_of(const Word<E> (&table)[N], E value) {
  for (const auto &entry : table) {
    if (entry.value == value) {
      return entry.word;
    }
  }
  return "";
}

bool valid_ipv4(const char *text) {
  int octets = 0;
  const char *p = text;
  while (octets < IPV4_OCTETS) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    char *end = nullptr;
    const long value = strtol(p, &end, 10);
    if (value > MAX_OCTET || end - p > 3) {
      return false;
    }
    octets++;
    p = end;
    if (octets < IPV4_OCTETS) {
      if (*p != '.') {
        return false;
      }
      p++;
    }
  }
  return *p == '\0';
}

bool parse_long(const char *text, long min, long max, long *out) {
  if (*text == '\0') {
    return false;
  }
  char *end = nullptr;
  const long value = strtol(text, &end, 10);
  if (*end != '\0' || value < min || value > max) {
    return false;
  }
  *out = value;
  return true;
}

bool apply_pair(GroupSpec *spec, const char *key, const char *value) {
  long number = 0;
  if (strcmp(key, "role") == 0) {
    return lookup(ROLES, value, &spec->role);
  }
  if (strcmp(key, "peer") == 0) {
    if (*value != '\0' && !valid_ipv4(value)) {
      return false;
    }
    snprintf(spec->peer, sizeof(spec->peer), "%s", value);
    return true;
  }
  if (strcmp(key, "port") == 0) {
    if (!parse_long(value, 1, MAX_PORT, &number)) {
      return false;
    }
    spec->port = (uint16_t) number;
    return true;
  }
  if (strcmp(key, "channel") == 0) {
    return lookup(CHANNELS, value, &spec->channel);
  }
  if (strcmp(key, "clock") == 0) {
    return lookup(CLOCKS, value, &spec->clock);
  }
  if (strcmp(key, "delay") == 0) {
    if (!parse_long(value, 0, MAX_DELAY_US, &number)) {
      return false;
    }
    spec->delay_us = (int32_t) number;
    return true;
  }
  if (strcmp(key, "name") == 0) {
    if (strlen(value) > GROUP_NAME_MAX_LEN) {
      return false;
    }
    snprintf(spec->name, sizeof(spec->name), "%s", value);
    return true;
  }
  if (strcmp(key, "standalone") == 0) {
    if (strcmp(value, "1") != 0 && strcmp(value, "0") != 0) {
      return false;
    }
    spec->standalone = value[0] == '1';
    return true;
  }
  return false;
}

}  // namespace

bool group_spec_parse(const char *text, GroupSpec *out) {
  if (text == nullptr || out == nullptr || *text == '\0' || strlen(text) > GROUP_SPEC_MAX_LEN) {
    return false;
  }
  GroupSpec spec;
  if (strcmp(text, STANDALONE_TEXT) == 0) {
    *out = spec;
    return true;
  }
  char buf[GROUP_SPEC_MAX_LEN + 1];
  snprintf(buf, sizeof(buf), "%s", text);
  bool has_role = false;
  char *save = nullptr;
  for (char *item = strtok_r(buf, ";", &save); item != nullptr; item = strtok_r(nullptr, ";", &save)) {
    char *eq = strchr(item, '=');
    if (eq == nullptr) {
      return false;
    }
    *eq = '\0';
    if (!apply_pair(&spec, item, eq + 1)) {
      return false;
    }
    has_role = has_role || strcmp(item, "role") == 0;
  }
  if (!has_role || (spec.role == PeerRole::PRIMARY && spec.peer[0] == '\0')) {
    return false;
  }
  *out = spec;
  return true;
}

size_t group_spec_format(const GroupSpec &spec, char *out, size_t capacity) {
  if (out == nullptr || capacity == 0) {
    return 0;
  }
  if (spec.role == PeerRole::STANDALONE) {
    const int len = snprintf(out, capacity, "%s", STANDALONE_TEXT);
    return (len > 0 && (size_t) len < capacity) ? (size_t) len : 0;
  }
  int len = snprintf(out, capacity, "role=%s", word_of(ROLES, spec.role));
  auto append = [&](const char *fmt, auto value) {
    if (len < 0 || (size_t) len >= capacity) {
      return;
    }
    const int more = snprintf(out + len, capacity - (size_t) len, fmt, value);
    len = more < 0 ? -1 : len + more;
  };
  if (spec.peer[0] != '\0') {
    append(";peer=%s", spec.peer);
  }
  if (spec.port != GROUP_DEFAULT_PORT) {
    append(";port=%u", (unsigned) spec.port);
  }
  if (spec.role == PeerRole::PRIMARY) {
    append(";channel=%s", word_of(CHANNELS, spec.channel));
    if (spec.name[0] != '\0') {
      append(";name=%s", spec.name);
    }
  } else {
    append(";clock=%s", word_of(CLOCKS, spec.clock));
    append(";delay=%ld", (long) spec.delay_us);
    append(";standalone=%d", spec.standalone ? 1 : 0);
  }
  if (len < 0 || (size_t) len >= capacity) {
    out[0] = '\0';
    return 0;
  }
  return (size_t) len;
}

}  // namespace airplay_receiver
}  // namespace esphome
