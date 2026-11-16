#pragma once

#include "Client.h"

// TCP client backed by lwIP sockets (IDF). Untested on hardware.
class WiFiClient : public Client {
 public:
  WiFiClient() = default;
  ~WiFiClient() override;

  int connect(IPAddress ip, uint16_t port) override;
  int connect(const char* host, uint16_t port) override;
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size) override;
  int peek() override;
  void flush() override;
  void stop() override;
  uint8_t connected() override;
  operator bool() override { return connected() != 0; }

  void setTimeout(uint32_t ms) { timeoutMs_ = ms; }
  uint32_t getTimeout() const { return timeoutMs_; }
  void setConnectionTimeout(uint32_t ms) { timeoutMs_ = ms; }
  int fd() const { return fd_; }

 private:
  int fd_ = -1;
  uint32_t timeoutMs_ = 30000;
};
