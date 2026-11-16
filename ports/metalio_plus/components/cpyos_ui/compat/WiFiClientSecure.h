#pragma once

#include "WiFiClient.h"

// TLS client placeholder for the Plus port.
//
// The IDF build has no wolfSSL component yet; every connect() fails loudly so
// an HTTPS-dependent activity reports a network error instead of silently
// sending plaintext. Replacing this with esp-tls/wolfSSL is the next network
// milestone.
struct esp_tls;

class WiFiClientSecure : public Client {
 public:
  WiFiClientSecure() = default;
  ~WiFiClientSecure() override;

  void setInsecure() { insecure_ = true; }
  void setCACert(const char* /*rootCA*/) {}
  void setCertificate(const char* /*clientCert*/) {}
  void setPrivateKey(const char* /*clientKey*/) {}
  void setHandshakeTimeout(unsigned long /*seconds*/) {}

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

 private:
  esp_tls* tls_ = nullptr;
  bool insecure_ = false;
  bool open_ = false;
  int pushback_ = -1;
};
