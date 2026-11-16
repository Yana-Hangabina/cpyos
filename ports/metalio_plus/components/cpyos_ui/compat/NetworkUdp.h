#pragma once

// Arduino NetworkUDP surface for the Plus port. The web-server activity is
// disabled on this port (CROSSPOINT_PORT_NO_NETWORK_SHELL); this class only
// has to satisfy headers that still declare an instance.

#include <cstddef>
#include <cstdint>

#include "IPAddress.h"

class NetworkUDP {
 public:
  uint8_t begin(uint16_t /*port*/) { return 0; }
  void stop() {}
  int beginPacket(IPAddress, uint16_t) { return 0; }
  int beginPacket(const char*, uint16_t) { return 0; }
  size_t write(const uint8_t*, size_t) { return 0; }
  size_t write(uint8_t) { return 0; }
  int endPacket() { return 0; }
  int parsePacket() { return 0; }
  int read() { return -1; }
  int read(uint8_t*, size_t) { return 0; }
  int available() { return 0; }
  IPAddress remoteIP() { return IPAddress(); }
  uint16_t remotePort() { return 0; }
};
