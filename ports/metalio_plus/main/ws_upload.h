#pragma once

// WebSocket upload server on port 81 (Calibre plugin / fast file manager path).
// Protocol: docs/webserver-endpoints.md "WebSocket Upload".
namespace ws_upload {

bool start();
void stop();
bool running();

// Snapshot for UI pages. Reads are atomic; `completed` increments after each
// successful upload so a page can show a one-shot confirmation.
struct Status {
    bool active;
    unsigned long long received;
    unsigned long long total;
    unsigned int completed;
};
Status status();

}  // namespace ws_upload
