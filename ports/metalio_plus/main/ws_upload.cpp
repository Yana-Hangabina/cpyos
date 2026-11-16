// WebSocket upload server on port 81 — the Calibre plugin / fast-upload path.
//
// The protocol is documented in docs/webserver-endpoints.md:
//   START:<filename>:<size>:<path> -> READY -> binary chunks
//   -> PROGRESS:<received>:<total> every 64 KiB (and at completion) -> DONE.
// Errors use ERROR:<message> with the documented wording. One upload at a time;
// bytes land in "<final>.part" and are renamed only after the declared size has
// been received. A disconnect/error removes the partial file.
//
// UNTESTED ON HARDWARE.

#include "ws_upload.h"

#include "transfer_paths.h"
#include "ws_protocol.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace ws_upload {
namespace {

constexpr const char* TAG = "PLUS-WS";
constexpr size_t kMaxFrame = 256 * 1024;
constexpr uint64_t kProgressEvery = 64 * 1024;

httpd_handle_t s_server = nullptr;

struct Upload {
    bool active = false;
    int fd = -1;
    int sock = -1;
    std::string temp;
    std::string final;
    uint64_t expected = 0;
    uint64_t received = 0;
    uint64_t nextProgress = kProgressEvery;
};

Upload s_upload;
std::atomic<bool> s_active{false};
std::atomic<unsigned long long> s_received{0};
std::atomic<unsigned long long> s_total{0};
std::atomic<unsigned int> s_completed{0};

void resetUpload(bool removeTemp) {
    if (s_upload.fd >= 0) close(s_upload.fd);
    if (removeTemp && !s_upload.temp.empty()) unlink(s_upload.temp.c_str());
    s_upload = Upload{};
    s_active.store(false, std::memory_order_relaxed);
    s_received.store(0, std::memory_order_relaxed);
    s_total.store(0, std::memory_order_relaxed);
}

bool sendText(httpd_req_t* req, const char* text) {
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
    frame.len = std::strlen(text);
    return httpd_ws_send_frame(req, &frame) == ESP_OK;
}

bool sendProgress(httpd_req_t* req) {
    char text[96];
    snprintf(text, sizeof(text), "PROGRESS:%llu:%llu", static_cast<unsigned long long>(s_upload.received),
             static_cast<unsigned long long>(s_upload.expected));
    return sendText(req, text);
}

void finishUpload(httpd_req_t* req) {
    if (s_upload.fd >= 0) {
        close(s_upload.fd);
        s_upload.fd = -1;
    }
    unlink(s_upload.final.c_str());
    const bool renamed = rename(s_upload.temp.c_str(), s_upload.final.c_str()) == 0;
    if (!renamed) {
        unlink(s_upload.temp.c_str());
        sendText(req, "ERROR:Failed to create file");
        s_upload = Upload{};
        return;
    }
    s_completed.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGI(TAG, "uploaded %s (%llu bytes)", s_upload.final.c_str(),
             static_cast<unsigned long long>(s_upload.received));
    sendProgress(req);
    sendText(req, "DONE");
    s_upload = Upload{};
}

bool startUpload(httpd_req_t* req, const ws_upload::Start& start) {
    if (!web_transfer::paths::safe(start.path)) {
        sendText(req, "ERROR:Invalid START format");
        return false;
    }
    const std::string dirReal = web_transfer::paths::toReal(start.path);
    struct stat dirStat = {};
    if (stat(dirReal.c_str(), &dirStat) != 0 || !S_ISDIR(dirStat.st_mode)) {
        sendText(req, "ERROR:Failed to create file");
        return false;
    }
    std::string logical = start.path;
    if (logical.empty() || logical.back() != '/') logical += '/';
    logical += start.filename;
    if (!web_transfer::paths::safe(logical)) {
        sendText(req, "ERROR:Invalid START format");
        return false;
    }
    const std::string finalPath = web_transfer::paths::toReal(logical);
    const std::string temp = finalPath + ".part";
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        sendText(req, "ERROR:Failed to create file");
        return false;
    }
    s_upload = Upload{};
    s_upload.active = true;
    s_active.store(true, std::memory_order_relaxed);
    s_total.store(start.size, std::memory_order_relaxed);
    s_received.store(0, std::memory_order_relaxed);
    s_upload.fd = fd;
    s_upload.sock = httpd_req_to_sockfd(req);
    s_upload.temp = temp;
    s_upload.final = finalPath;
    s_upload.expected = start.size;
    s_upload.nextProgress = kProgressEvery;
    if (start.size == 0) {
        finishUpload(req);
        return true;
    }
    sendText(req, "READY");
    return true;
}

esp_err_t wsHandler(httpd_req_t* req) {
    const int sock = httpd_req_to_sockfd(req);
    // ONE frame per invocation: esp_http_server parses each frame's first byte
    // (and therefore its opcode) before calling the handler again. An internal
    // receive loop would reuse a stale ws_type and shift the frame boundary by
    // one byte, which is exactly the "WS frame is not properly masked" storm
    // seen when this looped.
    httpd_ws_frame_t frame = {};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recv_frame failed: err=%d", static_cast<int>(err));
        if (s_upload.active && s_upload.sock == sock) resetUpload(true);
        return ESP_OK;
    }
    if (frame.len == 0) return ESP_OK;
    if (frame.len > kMaxFrame) {
        sendText(req, "ERROR:Frame too large");
        if (s_upload.active && s_upload.sock == sock) resetUpload(true);
        return ESP_OK;
    }
    std::unique_ptr<uint8_t[]> payload(new (std::nothrow) uint8_t[frame.len + 1]);
    if (!payload) {
        sendText(req, "ERROR:Out of memory");
        if (s_upload.active && s_upload.sock == sock) resetUpload(true);
        return ESP_OK;
    }
    frame.payload = payload.get();
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        if (s_upload.active && s_upload.sock == sock) resetUpload(true);
        return ESP_OK;
    }
    payload[frame.len] = '\0';

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        const std::string message(reinterpret_cast<const char*>(payload.get()), frame.len);
        if (message.rfind("START:", 0) != 0) return ESP_OK;
        if (s_upload.active) {
            sendText(req, "ERROR:Upload already in progress");
            return ESP_OK;
        }
        ws_upload::Start start;
        if (!parseStart(message, start)) {
            sendText(req, "ERROR:Invalid START format");
            return ESP_OK;
        }
        startUpload(req, start);
    } else if (frame.type == HTTPD_WS_TYPE_BINARY) {
        if (!s_upload.active || s_upload.sock != sock) {
            sendText(req, "ERROR:No upload in progress");
            return ESP_OK;
        }
        if (s_upload.received + frame.len > s_upload.expected) {
            sendText(req, "ERROR:Upload overflow");
            resetUpload(true);
            return ESP_OK;
        }
        size_t offset = 0;
        while (offset < frame.len) {
            const ssize_t written = write(s_upload.fd, payload.get() + offset, frame.len - offset);
            if (written <= 0) {
                ESP_LOGE(TAG, "write failed: errno=%d offset=%u len=%u", errno, static_cast<unsigned>(offset),
                         static_cast<unsigned>(frame.len));
                sendText(req, "ERROR:Write failed - disk full?");
                resetUpload(true);
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (!s_upload.active) return ESP_OK;
        s_upload.received += frame.len;
        s_received.store(s_upload.received, std::memory_order_relaxed);
        if (s_upload.received >= s_upload.expected) {
            finishUpload(req);
        } else if (s_upload.received >= s_upload.nextProgress) {
            sendProgress(req);
            s_upload.nextProgress = ((s_upload.received / kProgressEvery) + 1) * kProgressEvery;
        }
    }
    return ESP_OK;
}

}  // namespace

bool start() {
    if (s_server != nullptr) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    // The HTTP file server owns most of CONFIG_LWIP_MAX_SOCKETS; one Calibre
    // upload at a time needs two client sockets plus the control socket.
    config.ctrl_port = 32769;
    config.max_open_sockets = 2;
    config.max_uri_handlers = 2;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(TAG, "httpd_start(81) failed");
        return false;
    }
    httpd_uri_t uri = {};
    uri.uri = "/";
    uri.method = HTTP_GET;
    uri.handler = wsHandler;
    uri.user_ctx = nullptr;
    uri.is_websocket = true;
    uri.handle_ws_control_frames = false;  // IDF handles ping/pong/close
    if (httpd_register_uri_handler(s_server, &uri) != ESP_OK) {
        httpd_stop(s_server);
        s_server = nullptr;
        ESP_LOGE(TAG, "websocket handler registration failed");
        return false;
    }
    ESP_LOGI(TAG, "upload websocket ready on port 81");
    return true;
}

void stop() {
    if (s_server == nullptr) return;
    if (s_upload.active) resetUpload(true);
    httpd_stop(s_server);
    s_server = nullptr;
}

bool running() { return s_server != nullptr; }

Status status() {
    Status out{};
    out.active = s_active.load(std::memory_order_relaxed);
    out.received = s_received.load(std::memory_order_relaxed);
    out.total = s_total.load(std::memory_order_relaxed);
    out.completed = s_completed.load(std::memory_order_relaxed);
    return out;
}

}  // namespace ws_upload
