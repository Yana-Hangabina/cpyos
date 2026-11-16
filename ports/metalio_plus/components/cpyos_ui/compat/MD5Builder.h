#pragma once

// Arduino-core `MD5Builder` for the IDF port.
//
// IDF 6.1 ships mbedTLS 4.x, which no longer exposes the legacy public
// <mbedtls/md5.h>. MD5 here is a protocol compatibility hash (KOReader
// credentials, book identities), so the algorithm is implemented locally
// instead of depending on a crypto build option.
//
// Only the subset CPYOS uses is implemented: begin() / add(bytes|text) /
// calculate() / getBytes() / toString(). MD5 is used for KOReader credentials
// and book identities, matching the reference implementation's behaviour.

#include <cstddef>
#include <cstdint>

#include "WString.h"

class MD5Builder {
 public:
  MD5Builder() = default;

  void begin();
  void add(const uint8_t *data, uint16_t len);
  void add(const char *text);
  void calculate();
  void getBytes(uint8_t *out) const;
  String toString() const;

 private:
  uint32_t state_[4] = {0};
  uint64_t length_ = 0;
  uint8_t block_[64] = {0};
  size_t blockLen_ = 0;
  uint8_t digest_[16] = {0};
  bool started_ = false;
  bool finished_ = false;
};
