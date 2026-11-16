#pragma once

// Arduino Wire surface for the Plus port, backed by the BSP-owned shared I2C1
// bus (SDA=GPIO0, SCL=GPIO1). The BSP initialises that bus before the UI starts;
// `begin()` therefore ignores the pins a ported driver passes and only marks the
// bus usable. One device transaction at a time is enforced by the BSP mutex.
#include <cstddef>
#include <cstdint>

class TwoWire {
 public:
  void begin() {}
  void begin(int, int, uint32_t = 0) {}
  void setClock(uint32_t) {}

  void beginTransmission(uint8_t address);
  // stop=false defers the write so a following requestFrom() can issue a
  // repeated-start read (the pattern the ported sensor drivers use).
  uint8_t endTransmission(bool stop = true);
  size_t write(uint8_t value);
  size_t write(const uint8_t* data, size_t len);
  uint8_t requestFrom(uint8_t address, uint8_t len, bool stop = true);
  int available();
  int read();

 private:
  static constexpr size_t kBufferSize = 32;
  uint8_t tx_[kBufferSize] = {};
  size_t txLen_ = 0;
  uint8_t txAddr_ = 0;
  bool pendingWrite_ = false;
  uint8_t rx_[kBufferSize] = {};
  size_t rxLen_ = 0;
  size_t rxPos_ = 0;
};

extern TwoWire Wire;
extern TwoWire Wire1;
