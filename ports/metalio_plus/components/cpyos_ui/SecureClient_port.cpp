// freeink::SecureClient on IDF esp_tls.
//
// The reference build compiles SecureNet with wolfSSL for TLS 1.3. The Plus
// port links no wolfSSL: this implementation provides the same Arduino Client
// surface on IDF's esp_tls (mbedTLS), which is what SecureHttpClient, the OTA
// updater, the browser and KOReader sync call. TLS 1.3 availability follows
// CONFIG_MBEDTLS_SSL_PROTO_TLS1_3.
//
// UNTESTED ON HARDWARE.

#include <SecureClient.h>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_tls.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/select.h>

namespace freeink {

// SecureHttpClient uses this as a cheap preflight for the plain-HTTP path.
// ESP-IDF's esp_tls performs the actual DNS/TLS resolution in connect(); an
// empty host is the only input that can be rejected here.
bool hostResolves(const char* host) { return host != nullptr && host[0] != '\0'; }

namespace {
constexpr const char* TAG = "PLUS-TLS";

// The SDK header has no pushback/receive buffer member; one client is used
// per request path at a time, so a single file-static window serves peek() and
// available(). ESP-IDF's esp_tls only decrypts socket bytes during read(), so
// available() must pull them in when the socket says data is waiting —
// SecureHttpClient::readLine() otherwise polls available()==0 until timeout.
constexpr size_t kRxWindow = 512;
uint8_t g_rx[kRxWindow];
size_t g_rxLen = 0;
size_t g_rxPos = 0;
}

SecureClient::~SecureClient() { stop(); }

void SecureClient::setCACert(const char* rootCA) { _rootCA = rootCA; }

void SecureClient::setInsecure() { _insecure = true; }

bool SecureClient::tls13Available() {
#if defined(CONFIG_MBEDTLS_SSL_PROTO_TLS1_3)
    return true;
#else
    return false;
#endif
}

int SecureClient::connect(IPAddress ip, uint16_t port) {
    char host[16];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(host, port);
}

int SecureClient::connect(const char* host, uint16_t port) {
    stop();
    if (host == nullptr) return 0;
    if (abortRequested()) return 0;

    esp_tls_cfg_t cfg{};
    // SecureHttpClient calls setTimeout() on the SecureClient itself; the
    // transport keeps its own (unrelated) timeout. Reading _transport here made
    // cfg.timeout_ms 0, so esp_tls used no deadline and a stalled handshake hung
    // the fetch task forever. Use this client's timeout and always give
    // esp_tls a bound.
    const unsigned long clientTimeoutMs = getTimeout();
    cfg.timeout_ms = static_cast<int>(clientTimeoutMs > 0 ? clientTimeoutMs : 30000);
    if (!_insecure) {
        if (_rootCA != nullptr) {
            cfg.cacert_buf = reinterpret_cast<const unsigned char*>(_rootCA);
            cfg.cacert_bytes = static_cast<unsigned>(std::strlen(_rootCA));
        } else {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
    esp_tls_t* tls = esp_tls_init();
    if (tls == nullptr) return 0;
    if (abortRequested()) {
        esp_tls_conn_destroy(tls);
        return 0;
    }
    if (esp_tls_conn_new_sync(host, static_cast<int>(std::strlen(host)), port, &cfg, tls) != 1) {
        ESP_LOGW(TAG, "TLS connect to '%s:%u' failed", host, static_cast<unsigned>(port));
        esp_tls_conn_destroy(tls);
        return 0;
    }
    _ssl = tls;
    _connected = true;
    return 1;
}

size_t SecureClient::write(uint8_t b) { return write(&b, 1); }

size_t SecureClient::write(const uint8_t* buf, size_t size) {
    if (_ssl == nullptr || !_connected) return 0;
    const ssize_t n = esp_tls_conn_write(static_cast<esp_tls_t*>(_ssl), buf, size);
    if (n <= 0) {
        _connected = false;
        return 0;
    }
    return static_cast<size_t>(n);
}

int SecureClient::available() {
    if (_ssl == nullptr || !_connected) return 0;
    if (g_rxPos < g_rxLen) return static_cast<int>(g_rxLen - g_rxPos);
    const int buffered = esp_tls_get_bytes_avail(static_cast<esp_tls_t*>(_ssl));
    if (buffered > 0) return buffered;
    // Nothing decrypted yet: if the socket has data, pull one window in so the
    // caller's available()-driven read loop makes progress.
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(static_cast<esp_tls_t*>(_ssl), &sockfd) != ESP_OK || sockfd < 0) {
        return 0;
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(sockfd, &readable);
    struct timeval zero = {0, 0};
    if (select(sockfd + 1, &readable, nullptr, nullptr, &zero) <= 0) return 0;
    const ssize_t n = esp_tls_conn_read(static_cast<esp_tls_t*>(_ssl), g_rx, sizeof(g_rx));
    if (n <= 0) {
        if (n < 0) _connected = false;
        return 0;
    }
    g_rxLen = static_cast<size_t>(n);
    g_rxPos = 0;
    return static_cast<int>(g_rxLen);
}

int SecureClient::read() {
    uint8_t byte = 0;
    return read(&byte, 1) == 1 ? byte : -1;
}

int SecureClient::read(uint8_t* buf, size_t size) {
    if (_ssl == nullptr || !_connected || size == 0) return 0;
    if (g_rxPos < g_rxLen) {
        const size_t n = std::min(size, g_rxLen - g_rxPos);
        memcpy(buf, g_rx + g_rxPos, n);
        g_rxPos += n;
        return static_cast<int>(n);
    }
    const ssize_t n = esp_tls_conn_read(static_cast<esp_tls_t*>(_ssl), buf, size);
    if (n <= 0) {
        if (n < 0) _connected = false;
        return 0;
    }
    return static_cast<int>(n);
}

int SecureClient::peek() {
    if (available() <= 0) return -1;
    return g_rx[g_rxPos];
}

void SecureClient::flush() {}

void SecureClient::stop() {
    if (_ssl != nullptr) {
        esp_tls_conn_destroy(static_cast<esp_tls_t*>(_ssl));
        _ssl = nullptr;
    }
    _connected = false;
    g_rxLen = 0;
    g_rxPos = 0;
    _transport.stop();
}

uint8_t SecureClient::connected() { return (_connected && _ssl != nullptr) ? 1 : 0; }

bool SecureClient::abortRequested() {
    if (_shouldAbort && _shouldAbort()) {
        _aborted = true;
        return true;
    }
    return false;
}

int SecureClient::connectWithMethod(const char* host, uint16_t port, void*, const char*) {
    return connect(host, port);
}

}  // namespace freeink
