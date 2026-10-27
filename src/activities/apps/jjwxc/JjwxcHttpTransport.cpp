#include "JjwxcHttpTransport.h"

#include <HalPowerManager.h>
#include <Logging.h>

#include <cstring>

#include "network/WifiPowerSaveGuard.h"

namespace {

constexpr const char* kTag = "JJWXC";
constexpr int kTimeoutMs = 45000;
// Eight: the three fixed API headers, or the WAP set (User-Agent, Accept,
// Accept-Language, Cookie, Referer, Content-Type). Sized above what either path
// sends today, because the loop below drops what does not fit -- and a silently
// dropped Cookie header looks exactly like an expired session.
constexpr size_t kMaxHeaders = 8;

struct HeaderBridge {
  jjwxc::HeaderSink sink;
  void* ctx;
};

}  // namespace

bool JjwxcHttpTransport::networkReady() { return WeReadHttpClient::networkReady(); }

void JjwxcHttpTransport::resetSession() {
  for (auto& slot : slots_) {
    slot.session.reset();
    slot.host[0] = '\0';
  }
}

uint32_t JjwxcHttpTransport::reusedRequests() const {
  uint32_t total = 0;
  for (const auto& slot : slots_) total += slot.session.reusedRequests();
  return total;
}

WeReadHttpClient::Session& JjwxcHttpTransport::selectSession(const char* url) {
  char host[128] = {};
  if (!WeReadHttpClient::extractHttpsHost(url, host, sizeof(host))) host[0] = '\0';

  for (auto& slot : slots_) {
    if (slot.host[0] != '\0' && strcmp(slot.host, host) == 0) {
      slot.lastUse = ++useCounter_;
      return slot.session;
    }
  }
  for (auto& slot : slots_) {
    if (slot.host[0] == '\0') {
      snprintf(slot.host, sizeof(slot.host), "%s", host);
      slot.lastUse = ++useCounter_;
      return slot.session;
    }
  }
  // Every slot is bound to some other host. This is the three-host VIP rotation
  // (m -> static -> www -> m ...), where no slot can ever be reused: the next
  // request is another host change. Drop all of them before the handshake so
  // only the new TLS context is resident -- that peak is what pushed the heap to
  // its floor and starved the handshake. A two-host batch never reaches here.
  for (auto& slot : slots_) {
    slot.session.reset();
    slot.host[0] = '\0';
  }
  SessionSlot& chosen = slots_[0];
  snprintf(chosen.host, sizeof(chosen.host), "%s", host);
  chosen.lastUse = ++useCounter_;
  return chosen.session;
}

int JjwxcHttpTransport::request(const char* method, const char* url, const jjwxc::RequestHeader* headers,
                                size_t headerCount, const char* body, size_t bodyLen, jjwxc::BodySink onBody,
                                void* bodyCtx, jjwxc::HeaderSink onHeader, void* headerCtx) {
  if (!url || !url[0]) return jjwxc::kTransportNetwork;
  if (!WeReadHttpClient::networkReady()) return jjwxc::kTransportNoNetwork;

  // Keep the CPU at full speed for the exchange. The TLS handshake's ECDHE
  // scalar multiplication is CPU-bound and does not yield; at the 80 MHz idle
  // clock it can hold core 0 past the 5 s task-watchdog window, which is how a
  // retry run aborted (jjwxcJob running, IDLE0 starved). The lock ends with the
  // request, so the idle downclock still applies between chapters. It is
  // opportunistic (quiet): the render task also takes this single global lock,
  // and losing the race must not print an error for every progress frame.
  HalPowerManager::Lock powerLock(/*quiet=*/true);

  // After the early returns: an invalid URL is no reason to touch the radio.
  //
  // Without this, modem sleep powers the radio down between DTIM beacons and the
  // exchange degrades instead of failing cleanly. Measured on the device across one
  // session -- the same host, the same code, the same free heap, handshakes slowing
  // until they stopped completing at all:
  //
  //   handshake ok (auto): TLSv1.2 ... in 772 ms
  //   handshake ok (auto): TLSv1.2 ... in 5350 ms
  //   handshake ok (auto): TLSv1.2 ... in 4809 ms
  //   handshake timeout (auto): last err 2, transport up
  //   hostByName(): DNS Failed for 'm.jjwxc.net' with error '-54'
  //   connect(): socket error on fd 48, errno: 113, "Software caused connection abort"
  //
  // Once it reaches that state it does not recover on its own: every later request in
  // the session failed, including to a host that had answered minutes earlier, and each
  // one burned 37-70 s doing it. The UI can only report "网络错误", which sends the user
  // looking for a fault in the book they opened rather than in the radio.
  //
  // This is the project's own idiom for it -- PluginHttp, HttpDownloader, the web server
  // and both KOReader activities all hold the same guard for the same reason.
  const WifiPowerSaveGuard psGuard;

  WeReadHttpClient::Header mapped[kMaxHeaders];
  size_t mappedCount = 0;
  size_t usable = 0;
  for (size_t i = 0; i < headerCount; ++i) {
    if (!headers[i].name || !headers[i].value) continue;
    ++usable;
    if (mappedCount < kMaxHeaders) mapped[mappedCount++] = {headers[i].name, headers[i].value};
  }
  if (mappedCount < usable) {
    // Never silent: whichever header got dropped is unknowable from the response.
    LOG_ERR(kTag, "dropping %u request header(s), kMaxHeaders=%u", static_cast<unsigned>(usable - mappedCount),
            static_cast<unsigned>(kMaxHeaders));
  }

  WeReadHttpClient::RequestOptions options;
  options.method = method ? method : "GET";
  options.body = reinterpret_cast<const uint8_t*>(body);
  options.bodySize = body ? bodyLen : 0;
  options.headers = mapped;
  options.headerCount = mappedCount;
  options.timeoutMs = kTimeoutMs;
  options.readBuffer = ioBuffer_;
  options.readBufferSize = sizeof(ioBuffer_);

  // Body bridge: WeReadHttpClient hands over std::function, JjwxcApi uses a
  // plain function pointer + context so the library stays free of <functional>.
  bool aborted = false;
  size_t delivered = 0;
  const auto onData = [onBody, bodyCtx, &aborted, &delivered](const uint8_t* data, size_t len) {
    delivered += len;
    if (!onBody) return true;
    const bool keepGoing = onBody(bodyCtx, data, len);
    if (!keepGoing) aborted = true;
    return keepGoing;
  };

  HeaderBridge bridge{onHeader, headerCtx};
  const auto onHdr = [&bridge](const char* name, const char* value) {
    if (!bridge.sink || !name || !value) return;
    bridge.sink(bridge.ctx, name, strlen(name), value, strlen(value));
  };

  // One retry over a fresh connection, for a failure mode measured on the device:
  //
  //   [135039] wolfSSL TLS reused: host=m.jjwxc.net
  //   [135104] [ERR] wolfSSL status read failed
  //   [135108] [ERR] request failed status=-1 url=.../my/fav?p=1
  //   ...user retries by hand...
  //   [143597] wolfSSL TLS new: host=m.jjwxc.net
  //   [146332] shelf page 1: 30 books
  //
  // The session had been idle 67 seconds, the server had closed it, and `connected()`
  // still said yes -- so the write went into a dead socket and the status-line read
  // failed. Surfacing that as "网络错误" makes a keep-alive timeout look like a broken
  // network and costs the user a manual retry.
  //
  // Guarded three ways. Only a REUSED session is retried, because a fresh connection that
  // fails is a statement about conditions that are still true, and repeating the request
  // immediately does not change them. Only when nothing has been delivered to the body
  // sink, because a retry replays the whole response: after a partial body the caller
  // would see the first chunk twice, which in a chapter download means duplicated prose
  // in the book. And only for GET, because a POST is not idempotent -- the one POST this
  // app makes that matters is the login, and silently repeating it would spend a second
  // attempt against a risk-controlled account for a request the server may well have
  // already processed.
  //
  // The first guard was briefly widened to cover fresh connections too, on the theory that
  // the first request after the radio re-associates is the one most likely to fail. The
  // device measured that theory, and it was wrong in the expensive direction -- 111
  // seconds of a frozen progress bar to report a failure that was already certain after
  // the first 62:
  //
  //   [1740909] wolfSSL TLS new: host=m.jjwxc.net free=12067 largest=7156
  //   [1802983] [ERR] wolfSSL request connect failed
  //   [1802983] GET failed with nothing delivered (fresh session); reconnecting once
  //   [1852614] [ERR] wolfSSL request connect failed
  //
  // That connect did not fail because the network was briefly unsettled. It failed because
  // only 7,156 contiguous bytes were left for a handshake that needs more, so the retry ran
  // against the identical heap and failed identically. A reused-session failure is the
  // opposite case: the peer closed a connection `connected()` still reported as open, and
  // reconnecting genuinely does fix it -- which is the failure this retry was written for.
  const bool retryable = method != nullptr && strcmp(method, "GET") == 0;
  WeReadHttpClient::Session& session = selectSession(url);
  const uint32_t reusedBefore = session.reusedRequests();
  int status = 0;
  auto result = WeReadHttpClient::request(session, url, options, onData, onHdr, status);

  if (retryable && result == WeReadHttpClient::Result::NetworkError && delivered == 0 &&
      session.reusedRequests() != reusedBefore) {
    LOG_INF(kTag, "stale keep-alive session; reconnecting once for %.70s", url);
    session.reset();
    status = 0;
    result = WeReadHttpClient::request(session, url, options, onData, onHdr, status);
  }

  switch (result) {
    case WeReadHttpClient::Result::Ok:
      return status;
    case WeReadHttpClient::Result::Aborted:
      return aborted ? jjwxc::kTransportAborted : jjwxc::kTransportNetwork;
    case WeReadHttpClient::Result::NetworkError:
    default:
      LOG_ERR(kTag, "request failed status=%d url=%.60s", status, url);
      return jjwxc::kTransportNetwork;
  }
}
