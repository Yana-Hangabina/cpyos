#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Forward-declared: fetchUrl() takes a Stream& by reference. On-device this name
// arrives transitively via the SdFat/Arduino chain in <HalStorage.h>; declaring
// it here keeps the header self-sufficient (the host build's SdFat shim doesn't
// pull Arduino's Stream in transitively).
class Stream;

/**
 * HTTP client utility for fetching content and downloading files.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    UNAUTHORIZED,  // 401/403: callers holding a refreshable credential can retry
    NO_WIFI,       // no station link: refused before TLS (see the lwIP assert)
  };

  // Pre-flight floor for the ordinary allocator used by TLS (including
  // registered PSRAM). This is a heuristic, not a reservation or OOM guarantee.
  static constexpr uint32_t MIN_TLS_FREE_HEAP = 40000;
  static constexpr uint32_t MIN_TLS_MAX_ALLOC = 20000;
  static bool hasMemoryForTls();

  // HTTP status of the last request, or 0 when it never got a response (DNS,
  // connect, TLS, timeout). fetchUrl() says only success or failure; this lets a
  // caller tell a dead server from a catalog that wants a password.
  // Not thread-safe by design: one fetch runs at a time.
  static int lastStatus();

  // True when the last answer came from a different origin (scheme, host or
  // port) than the URL asked for, after a redirect. Credentials are withheld
  // from such a hop, so a 401 there is not a wrong password.
  static bool lastAnswerRedirected();

  // Measures how fast a URL delivers the first `maxBytes`. Used to pick an OTA
  // mirror: the body is aborted as soon as the sample is complete, so the
  // probe costs a few dozen KB rather than the whole file.
  struct ProbeResult {
    bool ok = false;
    size_t bytes = 0;
    uint32_t elapsedMs = 0;
    uint32_t firstByteMs = 0;
    uint32_t bytesPerSecond() const {
      return elapsedMs == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 1000) / elapsedMs);
    }
  };
  static ProbeResult probeUrl(const std::string& url, size_t maxBytes = 32768, int timeoutMs = 8000);

  using Header = std::pair<std::string, std::string>;

  /**
   * Fetch text content from a URL with optional credentials. `headers` are
   * added to the request, as for downloadToFile().
   */
  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", const std::vector<Header>& headers = {},
                       int timeoutMs = 0);

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   * `cancelFlag` is polled during DNS/TCP/TLS and every body chunk, so a
   * caller that shows a Cancel affordance can honour it before the connect
   * timeout expires.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", const std::vector<Header>& headers = {},
                       int timeoutMs = 0, const bool* cancelFlag = nullptr, DownloadError* error = nullptr);

  /**
   * Download a file to the SD card with optional credentials. `headers` are
   * added to the request (e.g. a Bearer Authorization), alongside any Basic
   * auth derived from username/password.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const std::vector<Header>& headers = {}, bool downgradeRedirectsToHttp = false,
                                      int timeoutMs = 0);
};
