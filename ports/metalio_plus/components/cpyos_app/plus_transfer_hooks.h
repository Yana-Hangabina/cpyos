#pragma once

// The transfer page lives in this component, but the HTTP/WebSocket servers are
// built into the shell component (main). The shell registers these hooks at
// startup so the activity layer never depends on the servers at link time.
namespace plus_transfer {

struct Status {
  bool serverRunning;
  unsigned long long received;
  unsigned long long total;
  unsigned int completed;
};

struct Hooks {
  bool (*start)();
  void (*stop)();
  Status (*status)();
  bool (*usbStart)();
};

bool available();
void setHooks(const Hooks& hooks);
bool start();
void stop();
Status status();
bool usbStart();

}  // namespace plus_transfer
