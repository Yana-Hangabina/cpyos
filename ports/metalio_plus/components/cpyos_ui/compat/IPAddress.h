#pragma once

#include <cstdint>
#include "WString.h"

class IPAddress {
 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { octets_[0] = a; octets_[1] = b; octets_[2] = c; octets_[3] = d; }
  explicit IPAddress(uint32_t raw) {
    octets_[0] = static_cast<uint8_t>(raw);
    octets_[1] = static_cast<uint8_t>(raw >> 8);
    octets_[2] = static_cast<uint8_t>(raw >> 16);
    octets_[3] = static_cast<uint8_t>(raw >> 24);
  }
  uint8_t operator[](int index) const { return octets_[index & 3]; }
  uint8_t& operator[](int index) { return octets_[index & 3]; }
  explicit operator uint32_t() const {
    return static_cast<uint32_t>(octets_[0]) | (static_cast<uint32_t>(octets_[1]) << 8) |
           (static_cast<uint32_t>(octets_[2]) << 16) | (static_cast<uint32_t>(octets_[3]) << 24);
  }
  bool operator==(const IPAddress& other) const {
    for (int i = 0; i < 4; ++i)
      if (octets_[i] != other.octets_[i]) return false;
    return true;
  }
  String toString() const;

 private:
  uint8_t octets_[4] = {0, 0, 0, 0};
};
