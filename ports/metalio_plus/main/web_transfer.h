#pragma once

namespace web_transfer {

// Starts the HTTP server on port 80 (idempotent). Returns false when the
// server cannot start. Call only after Wi-Fi is up (STA or SoftAP).
bool start();

// Stops the server if running.
void stop();

bool running();

}  // namespace web_transfer
