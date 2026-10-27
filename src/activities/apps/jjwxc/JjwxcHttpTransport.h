#pragma once

#include <JjwxcTransport.h>
#include <WeReadHttpClient.h>

#include <cstddef>
#include <cstdint>

// Adapts jjwxc::Transport onto CrossMux's existing HTTP stack.
//
// WeReadHttpClient is deliberately reused rather than duplicated: it already
// implements the subset of HTTP/1.1 the firmware needs over wolfSSL, does
// connection reuse per Session (one TLS handshake for a batch of requests), and
// exposes both a body callback and a header callback -- the latter is what lets
// JjwxcApi see the `accesskey`/`keystring` envelope headers. Reusing it costs
// zero extra Flash, which matters because the readpico build has only ~495 KB
// of headroom.
class JjwxcHttpTransport final : public jjwxc::Transport {
 public:
  bool networkReady() override;

  int request(const char* method, const char* url, const jjwxc::RequestHeader* headers, size_t headerCount,
              const char* body, size_t bodyLen, jjwxc::BodySink onBody, void* bodyCtx, jjwxc::HeaderSink onHeader,
              void* headerCtx) override;

  // Called before a burst of requests so the Session can be reused, and before
  // any long local write (SD I/O while holding a TLS session fragments the heap).
  void resetSession();
  uint32_t reusedRequests() const;

 private:
  static constexpr size_t kIoBufferSize = 4096;
  // Two sessions, keyed by host. A free-chapter run alternates app-cdn (body) and
  // www (comment counts), and that pair really does reuse both connections.
  //
  // A VIP chapter cycles THREE hosts (m page, static font, www comments), so the
  // slots evict each other on every request; keeping the survivor resident only
  // raised the peak during the next handshake. Measured on a retry run that
  // aborted in the task watchdog: internal free fell to 3.8 KB / largest 1 KB
  // with two TLS contexts live. selectSession() therefore drops every slot when a
  // third host arrives, which keeps exactly one context alive through that
  // handshake; a two-host batch still reuses both.
  static constexpr size_t kSessionSlots = 2;

  struct SessionSlot {
    WeReadHttpClient::Session session;
    char host[128] = {};
    uint32_t lastUse = 0;
  };

  // Picks the slot already bound to this URL's host, an unused one, or the
  // least-recently-used one (which is reset before it is rebound).
  WeReadHttpClient::Session& selectSession(const char* url);

  SessionSlot slots_[kSessionSlots];
  uint32_t useCounter_ = 0;
  uint8_t ioBuffer_[kIoBufferSize] = {};
};
