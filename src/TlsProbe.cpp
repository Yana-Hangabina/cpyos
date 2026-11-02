#include "TlsProbe.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(FREEINK_NET_WOLFSSL) && !defined(CROSSPOINT_EMULATED)
#include <HalPowerManager.h>
#include <JjwxcWapClient.h>
#include <SecureClient.h>

#if defined(BOARD_HAS_PSRAM)
#include <JjwxcCookies.h>

#include "activities/apps/jjwxc/JjwxcStore.h"
#endif

#include "network/WifiPowerSaveGuard.h"
#endif

namespace {

constexpr const char* kTag = "TLSPROBE";

size_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
size_t largestInternal() { return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

#if defined(FREEINK_NET_WOLFSSL) && !defined(CROSSPOINT_EMULATED)

struct ProbeCtx {
  char host[64] = {};
  char path[512] = {};
  int ballastKb = 0;
  bool http = false;
  bool useSid = false;
  bool useFull = false;
  bool keepAlive = false;
  std::atomic<bool> done{false};
  bool ok = false;
  uint32_t dnsTcpMs = 0;
  uint32_t tlsMs = 0;
  uint32_t totalMs = 0;
  uint32_t calls = 0;
  size_t freeBefore = 0;
  size_t largestBefore = 0;
  size_t freeEffective = 0;
  size_t largestEffective = 0;
  size_t freeAfter = 0;
  size_t largestAfter = 0;
  bool ballastOk = false;
  bool writeOk = false;
  bool statusSeen = false;
  int httpStatus = 0;
  uint32_t httpMs = 0;
  char statusLine[128] = {};
};

ProbeCtx g_ctx;
std::atomic<bool> g_busy{false};

uint64_t cookieClock() {
  const time_t t = time(nullptr);
  return t < 1577836800 ? 0 : static_cast<uint64_t>(t);
}

void resetProbeCtx() {
  g_ctx.host[0] = '\0';
  g_ctx.path[0] = '\0';
  g_ctx.ballastKb = 0;
  g_ctx.http = false;
  g_ctx.useSid = false;
  g_ctx.useFull = false;
  g_ctx.keepAlive = false;
  g_ctx.done.store(false);
  g_ctx.ok = false;
  g_ctx.dnsTcpMs = 0;
  g_ctx.tlsMs = 0;
  g_ctx.totalMs = 0;
  g_ctx.calls = 0;
  g_ctx.freeBefore = 0;
  g_ctx.largestBefore = 0;
  g_ctx.freeEffective = 0;
  g_ctx.largestEffective = 0;
  g_ctx.freeAfter = 0;
  g_ctx.largestAfter = 0;
  g_ctx.ballastOk = false;
  g_ctx.writeOk = false;
  g_ctx.statusSeen = false;
  g_ctx.httpStatus = 0;
  g_ctx.httpMs = 0;
  g_ctx.statusLine[0] = '\0';
}

// One HTTP/1.1 exchange on an already-open TLS client. Keeps the request bytes
// close to WeReadHttpClient's, because the point is to find out whether the m
// edge rejects the device's request rather than ours.
void httpExchange(freeink::SecureClient& client, ProbeCtx* ctx) {
#if defined(BOARD_HAS_PSRAM)
  jjwxc::CookieJar jar;
  const uint64_t now = cookieClock();
  const bool needJar = ctx->useSid || ctx->useFull;
  if (needJar && !JjwxcStore::loadCookies(jar, now)) {
    LOG_ERR(kTag, "cookie jar load failed");
  }
#endif

  char request[4096];
  size_t pos = 0;
  bool overflow = false;
  auto append = [&](const char* text) {
    const size_t n = text != nullptr ? strlen(text) : 0;
    if (pos + n + 1 > sizeof(request)) {
      overflow = true;
      return;
    }
    memcpy(request + pos, text, n);
    pos += n;
    request[pos] = '\0';
  };

  char line[768];
  snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\n", ctx->path);
  append(line);
  snprintf(line, sizeof(line), "Host: %s\r\n", ctx->host);
  append(line);
  snprintf(line, sizeof(line), "User-Agent: %s\r\n", jjwxc::kWapUserAgent);
  append(line);
  snprintf(line, sizeof(line), "Accept: %s\r\n", jjwxc::kWapAccept);
  append(line);
  snprintf(line, sizeof(line), "Accept-Language: %s\r\n", jjwxc::kWapAcceptLanguage);
  append(line);

#if defined(BOARD_HAS_PSRAM)
  if (ctx->useSid) {
    char sid[256] = {};
    if (jar.get("sid", sid, sizeof(sid), now) > 0) {
      snprintf(line, sizeof(line), "Cookie: sid=%s\r\n", sid);
      append(line);
    } else {
      LOG_ERR(kTag, "no sid in the device jar");
    }
  } else if (ctx->useFull) {
    char cookie[2048] = {};
    bool truncated = false;
    const size_t n = jar.buildHeader(ctx->host, ctx->path, true, cookie, sizeof(cookie), now, &truncated);
    if (n > 0) {
      snprintf(line, sizeof(line), "Cookie: %s\r\n", cookie);
      append(line);
      if (truncated) LOG_ERR(kTag, "cookie header truncated");
    } else {
      LOG_ERR(kTag, "cookie header empty");
    }
  }
#else
  if (ctx->useSid || ctx->useFull) {
    LOG_ERR(kTag, "cookie modes need the PSRAM/JJWXC store build");
  }
#endif

  append("Referer: https://m.jjwxc.net/book2/\r\n");
  append(ctx->keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
  append("\r\n");
  if (overflow) {
    LOG_ERR(kTag, "request does not fit");
    return;
  }

  size_t written = 0;
  while (written < pos) {
    const size_t n = client.write(reinterpret_cast<const uint8_t*>(request) + written, pos - written);
    if (n == 0) break;
    written += n;
  }
  ctx->writeOk = written == pos;
  if (!ctx->writeOk) {
    LOG_ERR(kTag, "http write stopped at %u/%u", static_cast<unsigned>(written), static_cast<unsigned>(pos));
    return;
  }

  const uint32_t started = millis();
  const uint32_t deadline = started + 15000;
  char status[128] = {};
  size_t statusLength = 0;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    const int value = client.read();
    if (value >= 0) {
      if (value == '\n') {
        if (statusLength > 0 && status[statusLength - 1] == '\r') --statusLength;
        status[statusLength] = '\0';
        ctx->statusSeen = true;
        break;
      }
      if (statusLength + 1 < sizeof(status)) status[statusLength++] = static_cast<char>(value);
      continue;
    }
    if (!client.connected() && client.available() == 0) break;
    delay(2);
  }
  ctx->httpMs = millis() - started;
  snprintf(ctx->statusLine, sizeof(ctx->statusLine), "%s", status);
  if (ctx->statusSeen) {
    int code = 0;
    if (sscanf(status, "HTTP/%*d.%*d %d", &code) == 1) ctx->httpStatus = code;
  }
}

void probeTask(void* param) {
  auto* ctx = static_cast<ProbeCtx*>(param);
  ctx->freeBefore = freeInternal();
  ctx->largestBefore = largestInternal();

  void* ballast = nullptr;
  if (ctx->ballastKb > 0) {
    ballast = heap_caps_malloc(static_cast<size_t>(ctx->ballastKb) * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ctx->ballastOk = ballast != nullptr;
    LOG_INF(kTag, "ballast %d KB: %s", ctx->ballastKb, ballast != nullptr ? "allocated" : "failed");
  }
  ctx->freeEffective = freeInternal();
  ctx->largestEffective = largestInternal();

  {
    HalPowerManager::Lock powerLock(/*quiet=*/true);
    const WifiPowerSaveGuard psGuard;
    freeink::SecureClient client;
    client.setInsecure();
    client.setTimeout(30000);

    struct Timing {
      uint32_t startMs = 0;
      uint32_t afterTcpMs = 0;
      uint32_t calls = 0;
    } timing;
    client.setAbortCallback([&timing]() {
      const uint32_t now = millis();
      if (timing.calls == 0) {
        timing.startMs = now;
      } else if (timing.calls == 1) {
        timing.afterTcpMs = now;
      }
      ++timing.calls;
      return false;
    });

    const uint32_t started = millis();
    ctx->ok = client.connect(ctx->host, 443);
    ctx->totalMs = millis() - started;
    ctx->calls = timing.calls;
    client.setAbortCallback(nullptr);
    if (timing.calls >= 2) {
      ctx->dnsTcpMs = timing.afterTcpMs - timing.startMs;
      ctx->tlsMs = ctx->totalMs - ctx->dnsTcpMs;
    }
    if (ctx->ok && ctx->http) httpExchange(client, ctx);
    client.stop();
  }

  if (ballast != nullptr) free(ballast);
  ctx->freeAfter = freeInternal();
  ctx->largestAfter = largestInternal();
  ctx->done.store(true);
  vTaskSuspend(nullptr);
}

void startAndWait() {
  TaskHandle_t task = nullptr;
  bool psram = false;
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(probeTask, "tlsProbe", 32 * 1024, &g_ctx, tskIDLE_PRIORITY, &task,
                                                  0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok == pdPASS) {
    psram = true;
  } else {
    task = nullptr;
    ok = xTaskCreatePinnedToCore(probeTask, "tlsProbe", 12 * 1024, &g_ctx, tskIDLE_PRIORITY, &task, 0);
  }
  if (ok != pdPASS) {
    g_busy.store(false);
    LOG_ERR(kTag, "could not start the probe task");
    return;
  }

  const uint32_t deadline = millis() + 90000;
  while (!g_ctx.done.load() && millis() < deadline) delay(20);
  if (!g_ctx.done.load()) {
    // Do not delete a task that may still be inside wolfSSL/heap; the static
    // context keeps the late result readable and g_busy refuses a second probe.
    LOG_ERR(kTag, "probe timed out; task left suspended");
    return;
  }

  char pathLog[64] = {};
  const size_t pathKeep = strcspn(g_ctx.path, "?");
  const size_t pathCopy = pathKeep < sizeof(pathLog) - 1 ? pathKeep : sizeof(pathLog) - 1;
  memcpy(pathLog, g_ctx.path, pathCopy);

  if (g_ctx.http) {
    LOG_INF(kTag,
            "http host=%s path=%s free_before=%u largest_before=%u dns+tcp=%lu ms tls=%lu ms http=%lu ms "
            "handshake_ok=%d write_ok=%d status_seen=%d status=%d line=\"%s\"",
            g_ctx.host, pathLog[0] != '\0' ? pathLog : "/", static_cast<unsigned>(g_ctx.freeBefore),
            static_cast<unsigned>(g_ctx.largestBefore), static_cast<unsigned long>(g_ctx.dnsTcpMs),
            static_cast<unsigned long>(g_ctx.tlsMs), static_cast<unsigned long>(g_ctx.httpMs), g_ctx.ok ? 1 : 0,
            g_ctx.writeOk ? 1 : 0, g_ctx.statusSeen ? 1 : 0, g_ctx.httpStatus, g_ctx.statusLine);
  } else {
    LOG_INF(kTag,
            "host=%s free_before=%u free_eff=%u free_after=%u largest_before=%u largest_eff=%u largest_after=%u "
            "ballast=%dKB/%d dns+tcp=%lu ms tls=%lu ms total=%lu ms ok=%d",
            g_ctx.host, static_cast<unsigned>(g_ctx.freeBefore), static_cast<unsigned>(g_ctx.freeEffective),
            static_cast<unsigned>(g_ctx.freeAfter), static_cast<unsigned>(g_ctx.largestBefore),
            static_cast<unsigned>(g_ctx.largestEffective), static_cast<unsigned>(g_ctx.largestAfter),
            g_ctx.ballastKb, g_ctx.ballastOk ? 1 : 0, static_cast<unsigned long>(g_ctx.dnsTcpMs),
            static_cast<unsigned long>(g_ctx.tlsMs), static_cast<unsigned long>(g_ctx.totalMs), g_ctx.ok ? 1 : 0);
  }
  if (g_ctx.calls < 2) {
    LOG_INF(kTag, "host=%s no TCP checkpoint (connect failed before TLS)", g_ctx.host);
  }

  if (psram) {
    vTaskDeleteWithCaps(task);
  } else {
    vTaskDelete(task);
  }
  g_busy.store(false);
}

#endif

}  // namespace

void runTlsProbe(const char* host, int ballastKb) {
#if !defined(FREEINK_NET_WOLFSSL) || defined(CROSSPOINT_EMULATED)
  (void)host;
  (void)ballastKb;
  LOG_ERR(kTag, "probe needs the wolfSSL build");
#else
  if (host == nullptr || host[0] == '\0') {
    LOG_ERR(kTag, "usage: CMD:TLSPROBE <host> [ballastKB]");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR(kTag, "Wi-Fi is not connected");
    return;
  }
  if (g_busy.load()) {
    LOG_ERR(kTag, "a probe is still running");
    return;
  }
  g_busy.store(true);
  resetProbeCtx();
  snprintf(g_ctx.host, sizeof(g_ctx.host), "%s", host);
  g_ctx.ballastKb = ballastKb > 0 ? ballastKb : 0;
  startAndWait();
#endif
}

void runHttpProbe(const char* host, const char* path, bool useSid, bool useFull, bool keepAlive) {
#if !defined(FREEINK_NET_WOLFSSL) || defined(CROSSPOINT_EMULATED)
  (void)host;
  (void)path;
  (void)useSid;
  (void)useFull;
  (void)keepAlive;
  LOG_ERR(kTag, "probe needs the wolfSSL build");
#else
  if (host == nullptr || host[0] == '\0' || path == nullptr || path[0] == '\0') {
    LOG_ERR(kTag, "usage: CMD:TLSPROBE http <host> <path> [sid|full] [keep]");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR(kTag, "Wi-Fi is not connected");
    return;
  }
  if (g_busy.load()) {
    LOG_ERR(kTag, "a probe is still running");
    return;
  }
  g_busy.store(true);
  resetProbeCtx();
  snprintf(g_ctx.host, sizeof(g_ctx.host), "%s", host);
  snprintf(g_ctx.path, sizeof(g_ctx.path), "%s", path);
  g_ctx.http = true;
  g_ctx.useSid = useSid;
  g_ctx.useFull = useFull;
  g_ctx.keepAlive = keepAlive;
  startAndWait();
#endif
}
