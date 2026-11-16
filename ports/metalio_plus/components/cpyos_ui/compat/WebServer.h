#pragma once

// Minimal Arduino-style WebServer on top of the ESP-IDF HTTP server.
//
// The private JJWXC component's cookie-import page was written against
// Arduino's WebServer (on/onNotFound/begin/send/arg). The Plus port has no
// Arduino network shell, so this compat layer provides exactly that surface on
// esp_http_server and keeps the shared component source unchanged.
//
// Supported: GET/POST routes, query and urlencoded form arguments, fixed
// responses. Not supported (not used by the component): chunked streaming,
// WebSockets, file uploads.

#include <WString.h>

#include <functional>
#include <memory>

#ifndef HTTP_GET
#define HTTP_GET "GET"
#endif
#ifndef HTTP_POST
#define HTTP_POST "POST"
#endif

using HTTPMethod = const char*;

class WebServer {
 public:
  using Handler = std::function<void()>;

  explicit WebServer(int port = 80);
  ~WebServer();

  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  void on(const String& uri, HTTPMethod method, Handler handler);
  void on(const char* uri, HTTPMethod method, Handler handler);
  void onNotFound(Handler handler);

  bool begin();
  void stop();
  void handleClient() {}

  void send(int code, const char* contentType, const String& body);
  void send(int code, const char* contentType, const char* body);

  String arg(const String& name) const;

  struct Impl;  // public so the C dispatch thunk in WebServer.cpp can name it
 private:
  std::unique_ptr<Impl> impl_;
};
