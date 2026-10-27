#pragma once

#include <cstddef>
#include <cstdint>

namespace jjwxc {

// Transport-level failures are returned as negative "status" values so a single
// int can carry either an HTTP status or a local error.
enum TransportError : int {
  kTransportNetwork = -1,    // connection/DNS/TLS failure
  kTransportAborted = -2,    // a sink returned false
  kTransportTooLarge = -3,  // response exceeded the caller's cap
  kTransportNoNetwork = -4,  // Wi-Fi not ready
};

struct RequestHeader {
  const char* name;
  const char* value;
};

// Body bytes are delivered as they arrive; returning false aborts the request
// (the transport then reports kTransportAborted).
using BodySink = bool (*)(void* ctx, const uint8_t* data, size_t len);

// Response headers. `name` is not case-normalised: JJWXC sends the envelope
// headers in lowercase (`accesskey` / `keystring`), so match case-insensitively.
using HeaderSink = void (*)(void* ctx, const char* name, size_t nameLen, const char* value, size_t valueLen);

// Implemented by the app layer (src/activities/apps/jjwxc) on top of
// WeReadHttpClient, so this library stays free of ESP/Arduino/wolfSSL includes
// and remains host-testable.
class Transport {
 public:
  virtual ~Transport() = default;

  virtual bool networkReady() = 0;

  // Returns the HTTP status code on success, or a negative TransportError.
  // `body`/`bodyLen` may be null/0 for GET.
  virtual int request(const char* method, const char* url, const RequestHeader* headers, size_t headerCount,
                      const char* body, size_t bodyLen, BodySink onBody, void* bodyCtx, HeaderSink onHeader,
                      void* headerCtx) = 0;
};

}  // namespace jjwxc
