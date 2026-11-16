#include "plus_transfer_hooks.h"

namespace plus_transfer {
namespace {
Hooks g_hooks{};
}

bool available() { return g_hooks.start != nullptr && g_hooks.stop != nullptr && g_hooks.status != nullptr; }

void setHooks(const Hooks& hooks) { g_hooks = hooks; }

bool start() { return available() && g_hooks.start(); }

void stop() {
  if (available()) g_hooks.stop();
}

Status status() {
  if (!available()) return Status{};
  return g_hooks.status();
}

bool usbStart() { return available() && g_hooks.usbStart != nullptr && g_hooks.usbStart(); }

}  // namespace plus_transfer
