#pragma once

// OPDS and Wi-Fi credential JSON APIs of the shared CPYOS web server, ported to
// the IDF httpd used by main/web_transfer.cpp. The Android/Web UI edits these
// stores directly; the device-side Settings pages use the same singletons.
//
// Passwords are never returned: the list only reports whether one is set, and a
// POST without a password field preserves the stored one (reference semantics).

#include <esp_http_server.h>

namespace web_stores {

bool registerRoutes(httpd_handle_t server);

}  // namespace web_stores
