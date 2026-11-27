#pragma once

// Host double for <Logging.h>: it keeps the lines so the test can count them.
// "One line per call site per boot" is half of this component's contract --
// silence would hide a broken callback, and a line per frame would bury
// everything else -- so the recording, not just the absence of a crash, is
// what is asserted.
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace logstub {

inline std::vector<std::string>& lines() {
  static std::vector<std::string> recorded;
  return recorded;
}

inline void record(const char* tag, const char* format, va_list args) {
  char buffer[256];
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  lines().emplace_back(std::string(tag) + ": " + buffer);
}

inline void error(const char* tag, const char* format, ...) {
  va_list args;
  va_start(args, format);
  record(tag, format, args);
  va_end(args);
}

inline void counted(const char* tag) { lines().push_back(std::string(tag) + ": counted"); }

}  // namespace logstub

#define LOG_ERR(tag, ...) logstub::error(tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) logstub::counted(tag)
#define LOG_INF(tag, ...) logstub::counted(tag)
#define LOG_DBG(tag, ...) logstub::counted(tag)
