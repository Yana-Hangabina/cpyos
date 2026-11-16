#pragma once

// Arduino-core Preferences for the IDF port, backed by NVS.
//
// CPYOS uses Preferences for the boot-language mirror, hardware settings and
// the trusted-clock anchor. Only the typed accessors the code calls are
// implemented; each maps to the equivalent NVS call.

#include <cstddef>
#include <cstdint>

#include "WString.h"

class Preferences {
 public:
  bool begin(const char *name, bool readOnly = false);
  void end();
  bool clear();

  bool isKey(const char *key);

  uint8_t getUChar(const char *key, uint8_t defaultValue = 0);
  size_t putUChar(const char *key, uint8_t value);

  uint32_t getUInt(const char *key, uint32_t defaultValue = 0);
  size_t putUInt(const char *key, uint32_t value);

  int64_t getLong64(const char *key, int64_t defaultValue = 0);
  size_t putLong64(const char *key, int64_t value);

  bool getBool(const char *key, bool defaultValue = false);
  size_t putBool(const char *key, bool value);

  String getString(const char *key, const String &defaultValue = String());
  size_t putString(const char *key, const String &value);
  size_t putString(const char *key, const char *value);

  size_t remove(const char *key);

 private:
  uint32_t handle_ = 0;
  bool open_ = false;
};
