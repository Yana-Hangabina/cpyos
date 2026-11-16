#include "WebServer.h"

// The Arduino-style HTTP_GET/HTTP_POST are strings; esp_http_server.h uses the
// same identifiers for its method enum. Drop the macros before including it.
#undef HTTP_GET
#undef HTTP_POST
#include <esp_http_server.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxQuery = 1024;
constexpr size_t kMaxBody = 8192;

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void urlDecodeInPlace(std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < text.size()) {
      const int hi = hexValue(text[i + 1]);
      const int lo = hexValue(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  text.swap(out);
}

void parsePairs(const std::string& text, std::map<std::string, std::string>& out) {
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('&', start);
    const std::string pair = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      std::string key = pair.substr(0, eq);
      std::string value = pair.substr(eq + 1);
      urlDecodeInPlace(key);
      urlDecodeInPlace(value);
      if (!key.empty()) out[key] = value;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

}  // namespace

struct WebServer::Impl {
  struct Route {
    std::string uri;
    httpd_method_t method;
    Handler handler;
  };

  int port = 80;
  httpd_handle_t server = nullptr;
  std::vector<Route> routes;
  std::vector<std::unique_ptr<httpd_uri_t>> registered;
  Handler notFound;
  httpd_req_t* current = nullptr;
  mutable std::map<std::string, std::string> currentArgs;
  mutable std::mutex argMutex;

  void collectArgs(httpd_req_t* req) {
    currentArgs.clear();
    char query[kMaxQuery];
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      parsePairs(query, currentArgs);
    }
    if (req->method == HTTP_POST) {
      const size_t len = std::min<size_t>(req->content_len, kMaxBody);
      if (len > 0) {
        std::string body(len, '\0');
        size_t got = 0;
        while (got < len) {
          const int n = httpd_req_recv(req, &body[got], len - got);
          if (n <= 0) break;
          got += static_cast<size_t>(n);
        }
        body.resize(got);
        parsePairs(body, currentArgs);
      }
    }
  }
};

namespace {

esp_err_t dispatch(httpd_req_t* req) {
  auto* impl = static_cast<WebServer::Impl*>(req->user_ctx);
  if (impl == nullptr) return ESP_FAIL;
  impl->current = req;
  impl->collectArgs(req);
  // Find the handler recorded for this exact method+uri.
  for (auto& route : impl->routes) {
    if (route.uri == req->uri && route.method == req->method) {
      if (route.handler) route.handler();
      impl->current = nullptr;
      return ESP_OK;
    }
  }
  if (impl->notFound) {
    impl->notFound();
    impl->current = nullptr;
    return ESP_OK;
  }
  impl->current = nullptr;
  httpd_resp_send_404(req);
  return ESP_OK;
}

}  // namespace

WebServer::WebServer(int port) : impl_(std::make_unique<Impl>()) { impl_->port = port; }

WebServer::~WebServer() { stop(); }

void WebServer::on(const String& uri, HTTPMethod method, Handler handler) {
  on(uri.c_str(), method, std::move(handler));
}

void WebServer::on(const char* uri, HTTPMethod method, Handler handler) {
  if (uri == nullptr || !handler) return;
  Impl::Route route;
  route.uri = uri;
  route.method = (method != nullptr && std::strcmp(method, "POST") == 0) ? HTTP_POST : HTTP_GET;
  route.handler = std::move(handler);
  impl_->routes.push_back(std::move(route));
}

void WebServer::onNotFound(Handler handler) { impl_->notFound = std::move(handler); }

bool WebServer::begin() {
  if (impl_->server != nullptr) return true;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = impl_->port;
  config.ctrl_port = impl_->port + 1;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  if (httpd_start(&impl_->server, &config) != ESP_OK) {
    impl_->server = nullptr;
    ESP_LOGE("WEB", "httpd_start failed on port %d", impl_->port);
    return false;
  }
  for (auto& route : impl_->routes) {
    auto entry = std::make_unique<httpd_uri_t>();
    entry->uri = route.uri.c_str();
    entry->method = route.method;
    entry->handler = &dispatch;
    entry->user_ctx = impl_.get();
    if (httpd_register_uri_handler(impl_->server, entry.get()) != ESP_OK) {
      ESP_LOGE("WEB", "register %s %s failed", route.method == HTTP_POST ? "POST" : "GET", route.uri.c_str());
    } else {
      impl_->registered.push_back(std::move(entry));
    }
  }
  return true;
}

void WebServer::stop() {
  if (impl_->server != nullptr) {
    httpd_stop(impl_->server);
    impl_->server = nullptr;
    impl_->registered.clear();
  }
}

void WebServer::send(int code, const char* contentType, const String& body) {
  send(code, contentType, body.c_str());
}

void WebServer::send(int code, const char* contentType, const char* body) {
  httpd_req_t* req = impl_->current;
  if (req == nullptr) return;
  char status[24];
  std::snprintf(status, sizeof(status), "%d", code);
  httpd_resp_set_status(req, status);
  if (contentType != nullptr) httpd_resp_set_type(req, contentType);
  httpd_resp_send(req, body != nullptr ? body : "", body != nullptr ? HTTPD_RESP_USE_STRLEN : 0);
}

String WebServer::arg(const String& name) const {
  std::lock_guard<std::mutex> lock(impl_->argMutex);
  const auto it = impl_->currentArgs.find(name.c_str());
  if (it == impl_->currentArgs.end()) return String();
  return String(it->second.c_str());
}
