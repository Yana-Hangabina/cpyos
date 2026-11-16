// Real I2C for the ported Arduino drivers (IMU today).
//
// The BSP owns the bus and its mutex. Arduino semantics are reproduced here:
//   beginTransmission(addr); write(reg); endTransmission(false);
//   requestFrom(addr, len)   -> one write/read transaction (repeated start)
//   beginTransmission(addr); write(reg); write(value); endTransmission()
//                            -> one plain write
// Anything bigger than the 32-byte scratch returns an error instead of
// silently truncating.

#include "Wire.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <metalio_i2c1.h>

namespace {
constexpr i2c_port_t kPort = I2C_NUM_1;
constexpr int kTimeoutMs = 100;
}  // namespace

void TwoWire::beginTransmission(uint8_t address) {
    txAddr_ = address;
    txLen_ = 0;
    pendingWrite_ = false;
}

uint8_t TwoWire::endTransmission(bool stop) {
    if (!stop) {
        // Keep the pointer for the requestFrom() that follows.
        pendingWrite_ = txLen_ > 0;
        return 0;
    }
    pendingWrite_ = false;
    if (txLen_ == 0) {
        return 2;
    }
    if (!metalio_i2c1_lock(kTimeoutMs)) {
        txLen_ = 0;
        return 2;
    }
    const esp_err_t err = metalio_i2c1_transmit_locked(kPort, txAddr_, tx_, txLen_, kTimeoutMs);
    metalio_i2c1_unlock();
    txLen_ = 0;
    return err == ESP_OK ? 0 : 2;
}

size_t TwoWire::write(uint8_t value) {
    if (txLen_ >= kBufferSize) {
        return 0;
    }
    tx_[txLen_++] = value;
    return 1;
}

size_t TwoWire::write(const uint8_t* data, size_t len) {
    size_t written = 0;
    while (written < len && txLen_ < kBufferSize) {
        tx_[txLen_++] = data[written++];
    }
    return written;
}

uint8_t TwoWire::requestFrom(uint8_t address, uint8_t len, bool) {
    if (len == 0 || len > kBufferSize) {
        return 0;
    }
    if (!metalio_i2c1_lock(kTimeoutMs)) {
        return 0;
    }
    esp_err_t err;
    if (pendingWrite_ && txLen_ == 1 && address == txAddr_) {
        // Repeated start: register pointer then read.
        err = metalio_i2c1_transmit_receive_locked(kPort, address, tx_, 1, rx_, len, kTimeoutMs);
    } else if (pendingWrite_ && txLen_ > 0 && address == txAddr_) {
        err = metalio_i2c1_transmit_locked(kPort, address, tx_, txLen_, kTimeoutMs);
        if (err == ESP_OK) {
            err = metalio_i2c1_receive_locked(kPort, address, rx_, len, kTimeoutMs);
        }
    } else {
        err = metalio_i2c1_receive_locked(kPort, address, rx_, len, kTimeoutMs);
    }
    metalio_i2c1_unlock();
    pendingWrite_ = false;
    txLen_ = 0;
    rxLen_ = err == ESP_OK ? len : 0;
    rxPos_ = 0;
    return static_cast<uint8_t>(rxLen_);
}

int TwoWire::available() { return static_cast<int>(rxLen_ - rxPos_); }

int TwoWire::read() { return rxPos_ < rxLen_ ? rx_[rxPos_++] : -1; }

TwoWire Wire;
TwoWire Wire1;
