// Arduino Client backends for the Metalio E-INK4 Plus.
//
// WiFiClient        plain TCP over lwIP sockets (non-blocking + select).
// WiFiClientSecure  TLS via IDF esp_tls; the certificate bundle is used when
//                   available, and setInsecure() falls back to esp_tls's
//                   no-verification mode (the reference build also ships
//                   setInsecure, documented as a risk).
//
// UNTESTED ON HARDWARE.

#include "WiFiClient.h"
#include "WiFiClientSecure.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_tls.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

static const char* TAG = "PLUS-NET";

namespace {
bool waitFd(int fd, bool forWrite, uint32_t timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeoutMs / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
    const int result = select(fd + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
    return result > 0;
}
}  // namespace

WiFiClient::~WiFiClient() { stop(); }

int WiFiClient::connect(const char* host, uint16_t port) {
    stop();
    if (host == nullptr) return 0;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0 || result == nullptr) {
        ESP_LOGW(TAG, "DNS failed for '%s'", host);
        return 0;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        freeaddrinfo(result);
        return 0;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno == EINPROGRESS) {
        if (!waitFd(fd, true, timeoutMs_)) {
            close(fd);
            return 0;
        }
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
            close(fd);
            return 0;
        }
    } else if (rc != 0) {
        close(fd);
        return 0;
    }
    fd_ = fd;
    return 1;
}

int WiFiClient::connect(IPAddress ip, uint16_t port) {
    char host[16];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(host, port);
}

size_t WiFiClient::write(const uint8_t* buf, size_t size) {
    if (fd_ < 0) return 0;
    size_t sent = 0;
    while (sent < size) {
        const ssize_t n = send(fd_, buf + sent, size - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!waitFd(fd_, true, timeoutMs_)) break;
        } else {
            stop();
            break;
        }
    }
    return sent;
}

size_t WiFiClient::write(uint8_t b) { return write(&b, 1); }

int WiFiClient::available() {
    if (fd_ < 0) return 0;
    int pending = 0;
    if (lwip_ioctl(fd_, FIONREAD, &pending) == 0 && pending > 0) return pending;
    // FIONREAD is not reliable on every lwIP build behind this VFS: with the
    // socket open and bytes already queued it can still answer 0. The SDK's
    // HTTP clients gate the header read on available() > 0, so an always-zero
    // answer turns every plain-HTTP response into a 60 s timeout (measured:
    // read() delivered 782 bytes while available() reported 0 on all 9 polls).
    // A pending-byte peek is what those polling loops actually need.
    uint8_t byte = 0;
    const ssize_t n = recv(fd_, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0) return 1;
    if (n == 0) {
        // Peer closed and nothing else is queued: let connected() observe it.
        stop();
    }
    return 0;
}

int WiFiClient::read() {
    uint8_t byte = 0;
    const ssize_t n = recv(fd_, &byte, 1, MSG_DONTWAIT);
    return n == 1 ? byte : -1;
}

int WiFiClient::read(uint8_t* buf, size_t size) {
    if (fd_ < 0) return 0;
    const ssize_t n = recv(fd_, buf, size, MSG_DONTWAIT);
    return n > 0 ? static_cast<int>(n) : 0;
}

int WiFiClient::peek() {
    if (fd_ < 0) return -1;
    uint8_t byte = 0;
    const ssize_t n = recv(fd_, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 1 ? byte : -1;
}

void WiFiClient::flush() {}

void WiFiClient::stop() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

uint8_t WiFiClient::connected() {
    if (fd_ < 0) return 0;
    uint8_t byte = 0;
    const ssize_t n = recv(fd_, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) {
        stop();
        return 0;
    }
    if (n > 0) return 1;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : 0;
}

// --- TLS -------------------------------------------------------------------

WiFiClientSecure::~WiFiClientSecure() { stop(); }

int WiFiClientSecure::connect(const char* host, uint16_t port) {
    stop();
    if (host == nullptr) return 0;
    esp_tls_cfg_t cfg{};
    if (!insecure_) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    tls_ = esp_tls_init();
    if (tls_ == nullptr) return 0;
    if (esp_tls_conn_new_sync(host, static_cast<int>(strlen(host)), port, &cfg, tls_) != 1) {
        ESP_LOGW(TAG, "TLS connect to '%s' failed", host);
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
        return 0;
    }
    open_ = true;
    return 1;
}

int WiFiClientSecure::connect(IPAddress ip, uint16_t port) {
    char host[16];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(host, port);
}

size_t WiFiClientSecure::write(const uint8_t* buf, size_t size) {
    if (tls_ == nullptr || !open_) return 0;
    const ssize_t n = esp_tls_conn_write(tls_, buf, size);
    if (n <= 0) {
        open_ = false;
        return 0;
    }
    return static_cast<size_t>(n);
}

size_t WiFiClientSecure::write(uint8_t b) { return write(&b, 1); }

int WiFiClientSecure::available() {
    if (tls_ == nullptr || !open_) return 0;
    return pushback_ >= 0 ? 1 : esp_tls_get_bytes_avail(tls_);
}

int WiFiClientSecure::read() {
    uint8_t byte = 0;
    return read(&byte, 1) == 1 ? byte : -1;
}

int WiFiClientSecure::read(uint8_t* buf, size_t size) {
    if (tls_ == nullptr || !open_ || size == 0) return 0;
    size_t offset = 0;
    if (pushback_ >= 0) {
        buf[0] = static_cast<uint8_t>(pushback_);
        pushback_ = -1;
        offset = 1;
    }
    if (offset == size) return static_cast<int>(offset);
    const ssize_t n = esp_tls_conn_read(tls_, buf + offset, size - offset);
    if (n < 0) {
        open_ = false;
        return offset == 0 ? 0 : static_cast<int>(offset);
    }
    if (n == 0) {
        open_ = false;
        return offset == 0 ? 0 : static_cast<int>(offset);
    }
    return static_cast<int>(offset + static_cast<size_t>(n));
}

int WiFiClientSecure::peek() {
    if (pushback_ < 0) {
        uint8_t byte = 0;
        if (read(&byte, 1) != 1) return -1;
        pushback_ = byte;
    }
    return pushback_;
}

void WiFiClientSecure::flush() {}

void WiFiClientSecure::stop() {
    if (tls_ != nullptr) {
        esp_tls_conn_destroy(tls_);
        tls_ = nullptr;
    }
    open_ = false;
    pushback_ = -1;
}

uint8_t WiFiClientSecure::connected() { return open_ && tls_ != nullptr ? 1 : 0; }
