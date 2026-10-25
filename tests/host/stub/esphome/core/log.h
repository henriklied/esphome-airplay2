#pragma once
// Host stand-in for ESPHome's logger: enough of the surface for audio_dsp.cpp.

#include <cstdarg>
#include <cstdio>

#define ESPHOME_LOG_LEVEL_INFO 3
#define ESPHOME_LOG_LEVEL_CONFIG 4

inline void esp_log_printf_(int level, const char *tag, int line, const char *format, ...) {
  (void) level;
  (void) line;
  std::fprintf(stderr, "[%s] ", tag);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fprintf(stderr, "\n");
}

#define ESP_LOGW(tag, ...) esp_log_printf_(2, tag, __LINE__, __VA_ARGS__)
