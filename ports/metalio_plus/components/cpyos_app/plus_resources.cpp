#include "plus_resources.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <SecureHttpClient.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <strings.h>
#include <sys/stat.h>

#include "FontInstaller.h"
#include "SdCardFontRegistry.h"
#include "SdCardFontSystem.h"
#include "plus_cloudzao_ids.h"
#include "plus_ota.h"

namespace plus_resources {
namespace {

constexpr const char* kPairPath = "/pairings";
constexpr const char* kTokenPath = "/device-tokens";
constexpr const char* kRefreshPath = "/device-tokens/refresh";
constexpr const char* kListPath = "/deliveries";
constexpr const char* kTokenFile = "/.crosspoint/cloudzao.json";
constexpr size_t kMaxListBodyBytes = 32768;
constexpr size_t kMaxResources = 48;
constexpr size_t kMaxFileNameBytes = 180;
constexpr int kRefreshLeadSeconds = 60;

struct Tokens {
  std::string access;
  std::string refresh;
  int64_t access_exp = 0;
  int64_t refresh_exp = 0;
  std::string device_code;
  std::string activation_code;
  int64_t pairing_exp = 0;
  int poll_seconds = 5;
  uint32_t last_poll_ms = 0;
  bool loaded = false;
};

Tokens g_tokens;

int64_t epochNow() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? static_cast<int64_t>(now) : 0;
}

std::string urlFor(const char* path) { return std::string(plus_cloudzao::kBaseUrl) + path; }

std::string readSmallFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("RES", path, file)) return {};
  std::string out;
  char buf[512];
  int n = 0;
  while ((n = file.read(buf, sizeof(buf))) > 0) {
    out.append(buf, static_cast<size_t>(n));
    if (out.size() > 8192) break;
  }
  file.close();
  return out;
}

bool writeSmallFile(const char* path, const std::string& text) {
  Storage.ensureDirectoryExists("/.crosspoint");
  const std::string tmp = std::string(path) + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("RES", tmp.c_str(), file)) return false;
  const bool ok = file.write(text.data(), text.size()) == text.size();
  file.close();
  if (!ok) return false;
  return Storage.replaceFile(tmp.c_str(), path);
}

bool loadTokens() {
  if (g_tokens.loaded) return true;
  g_tokens.loaded = true;
  const std::string raw = readSmallFile(kTokenFile);
  if (raw.empty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  bool ok = false;
  g_tokens.access = obfuscation::deobfuscateFromBase64(doc["access"] | "", 4096, &ok, nullptr);
  if (!ok) g_tokens.access.clear();
  ok = false;
  g_tokens.refresh = obfuscation::deobfuscateFromBase64(doc["refresh"] | "", 4096, &ok, nullptr);
  if (!ok) g_tokens.refresh.clear();
  ok = false;
  g_tokens.device_code = obfuscation::deobfuscateFromBase64(doc["device"] | "", 2048, &ok, nullptr);
  if (!ok) g_tokens.device_code.clear();
  g_tokens.activation_code = doc["activation"] | "";
  g_tokens.access_exp = doc["access_exp"] | static_cast<int64_t>(0);
  g_tokens.refresh_exp = doc["refresh_exp"] | static_cast<int64_t>(0);
  g_tokens.pairing_exp = doc["pair_exp"] | static_cast<int64_t>(0);
  g_tokens.poll_seconds = doc["poll"] | 5;
  return true;
}

bool saveTokens() {
  JsonDocument doc;
  doc["access"] = obfuscation::obfuscateToBase64(g_tokens.access);
  doc["refresh"] = obfuscation::obfuscateToBase64(g_tokens.refresh);
  doc["device"] = obfuscation::obfuscateToBase64(g_tokens.device_code);
  doc["activation"] = g_tokens.activation_code;
  doc["access_exp"] = g_tokens.access_exp;
  doc["refresh_exp"] = g_tokens.refresh_exp;
  doc["pair_exp"] = g_tokens.pairing_exp;
  doc["poll"] = g_tokens.poll_seconds;
  std::string text;
  serializeJson(doc, text);
  return writeSmallFile(kTokenFile, text);
}

void clearTokens() {
  g_tokens = Tokens{};
  g_tokens.loaded = true;
  Storage.remove(kTokenFile);
}

bool accessValid() {
  loadTokens();
  if (g_tokens.access.empty()) return false;
  const int64_t now = epochNow();
  if (now == 0) return true;  // no clock: use it, a 401 triggers a refresh
  return g_tokens.access_exp == 0 || now + kRefreshLeadSeconds < g_tokens.access_exp;
}

void parseTokenResponse(const JsonDocument& doc) {
  const char* access = doc["accessToken"] | "";
  const char* refresh = doc["refreshToken"] | "";
  if (access[0] != '\0') g_tokens.access = access;
  if (refresh[0] != '\0') g_tokens.refresh = refresh;
  const int64_t now = epochNow();
  const int access_in = doc["expiresIn"] | 0;
  const int refresh_in = doc["refreshExpiresIn"] | 0;
  if (now != 0 && access_in > 0) g_tokens.access_exp = now + access_in;
  if (now != 0 && refresh_in > 0) g_tokens.refresh_exp = now + refresh_in;
  g_tokens.device_code.clear();
  g_tokens.activation_code.clear();
  g_tokens.pairing_exp = 0;
  saveTokens();
}

struct HttpResult {
  int status = 0;
  std::string body;
  std::string error;
};

HttpResult httpRequest(const char* method, const std::string& url, const std::string& payload, const char* token,
                       const size_t max_body = kMaxListBodyBytes) {
  HttpResult out;
  freeink::SecureHttpClient http;
  http.setInsecure();
  http.setTimeout(25000);
  http.setFollowRedirects(2);
  http.setReuse(false);
  if (!http.begin(url.c_str())) {
    out.error = "cannot open the endpoint";
    return out;
  }
  http.addHeader("Accept", "application/json");
  char ua[80];
  std::snprintf(ua, sizeof(ua), "CPYOS-Plus/%s", plus_ota::currentVersion());
  http.addHeader("User-Agent", ua);
  if (token != nullptr && token[0] != '\0') {
    http.addHeader("Authorization", std::string("Bearer ") + token);
  }
  if (!payload.empty()) http.addHeader("Content-Type", "application/json");
  const auto sink = [&out, max_body](const uint8_t* data, const size_t len) {
    if (out.body.size() + len > max_body) return false;
    out.body.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  if (strcmp(method, "GET") == 0) {
    out.status = http.sendRequest("GET", nullptr, 0, sink);
  } else if (strcmp(method, "POST") == 0) {
    out.status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), sink);
  } else {
    out.status = http.sendRequest(method, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), sink);
  }
  http.end();
  return out;
}

bool refreshAccess() {
  loadTokens();
  if (g_tokens.refresh.empty()) return false;
  const int64_t now = epochNow();
  if (now != 0 && g_tokens.refresh_exp != 0 && now >= g_tokens.refresh_exp) {
    clearTokens();
    return false;
  }
  JsonDocument req;
  req["refreshToken"] = g_tokens.refresh;
  std::string payload;
  serializeJson(req, payload);
  const HttpResult r = httpRequest("POST", urlFor(kRefreshPath), payload, nullptr, 8192);
  LOG_INF("RES", "token refresh: http=%d body=%u B", r.status, static_cast<unsigned>(r.body.size()));
  if (r.status != 200) {
    if (r.status == 401) clearTokens();
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, r.body) != DeserializationError::Ok) return false;
  parseTokenResponse(doc);
  return !g_tokens.access.empty();
}

bool ensureAccess() {
  if (accessValid()) return true;
  return refreshAccess();
}

std::string jsonError(const JsonDocument& doc, const char* fallback) {
  const char* msg = doc["msg"] | "";
  if (msg[0] != '\0') return msg;
  const char* error = doc["error"] | "";
  if (error[0] != '\0') return error;
  return fallback;
}

bool parseU64(const JsonVariantConst& v, uint64_t& out) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s == nullptr || s[0] == '\0') return false;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(s, &end, 10);
    if (end == s) return false;
    out = static_cast<uint64_t>(value);
    return true;
  }
  if (v.is<uint64_t>()) {
    out = v.as<uint64_t>();
    return true;
  }
  if (v.is<int64_t>()) {
    const int64_t signed_value = v.as<int64_t>();
    if (signed_value < 0) return false;
    out = static_cast<uint64_t>(signed_value);
    return true;
  }
  return false;
}

std::string lower(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool endsWith(const std::string& value, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return value.size() >= n && lower(value.substr(value.size() - n)) == suffix;
}

std::string sanitizeComponent(std::string raw, const char* fallback) {
  const size_t slash = raw.find_last_of("/\\");
  if (slash != std::string::npos) raw.erase(0, slash + 1);
  std::string cleaned;
  cleaned.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c < 0x20 || c == 0x7f) continue;
    if (std::strchr("/\\:*?\"<>|", c) != nullptr) {
      cleaned.push_back('_');
    } else {
      cleaned.push_back(static_cast<char>(c));
    }
  }
  while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) cleaned.pop_back();
  while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '.')) cleaned.erase(cleaned.begin());
  if (cleaned.empty() || cleaned == "." || cleaned == "..") cleaned = fallback;
  if (cleaned.size() > kMaxFileNameBytes) {
    size_t cut = kMaxFileNameBytes;
    while (cut > 0 && (static_cast<unsigned char>(cleaned[cut]) & 0xc0) == 0x80) --cut;
    cleaned.resize(cut);
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) cleaned.pop_back();
    if (cleaned.empty()) cleaned = fallback;
  }
  return cleaned;
}

std::string sanitizeFamily(const std::string& raw, const std::string& resource_id) {
  std::string family;
  const size_t dot = raw.find_last_of('.');
  const std::string stem = dot == std::string::npos ? raw : raw.substr(0, dot);
  for (unsigned char c : stem) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      family.push_back(static_cast<char>(c));
    }
    if (family.size() >= 32) break;
  }
  if (family.empty()) {
    family = "Cloud";
    for (unsigned char c : resource_id) {
      if (std::isdigit(c)) family.push_back(static_cast<char>(c));
      if (family.size() >= 32) break;
    }
  }
  if (family.empty()) family = "CloudFont";
  if (!FontInstaller::isValidFamilyName(family.c_str())) family = "CloudFont";
  return family;
}

bool hasBookExtension(const std::string& name) {
  return endsWith(name, ".epub") || endsWith(name, ".txt") || endsWith(name, ".fb2") || endsWith(name, ".cbz") ||
         endsWith(name, ".xtc") || endsWith(name, ".md");
}

std::string appendBookExtension(std::string name, const std::string& format) {
  if (hasBookExtension(name)) return name;
  const std::string fmt = lower(format);
  if (fmt == "epub") return name + ".epub";
  if (fmt == "txt") return name + ".txt";
  return name;
}

std::string ensureDirectory(const char* path) {
  if (!Storage.exists(path)) Storage.ensureDirectoryExists(path);
  return path;
}

bool osPathIsSafe(const std::string& path) {
  return !path.empty() && path.size() < 240 && path[0] == '/' && path.find("..") == std::string::npos;
}

bool prepareBook(Item& item) {
  std::string file = sanitizeComponent(item.name.c_str(), "book");
  file = appendBookExtension(file, item.format.c_str());
  if (!hasBookExtension(file)) {
    item.state = ItemState::Unsupported;
    item.supported = false;
    return false;
  }
  ensureDirectory("/books");
  item.local_path = ("/books/" + file).c_str();
  item.supported = true;
  return osPathIsSafe(item.local_path.c_str());
}

bool prepareBadge(Item& item) {
  std::string file = sanitizeComponent(item.name.c_str(), "badge");
  if (file.find('.') == std::string::npos) file += ".png";
  ensureDirectory("/badges");
  item.local_path = ("/badges/" + file).c_str();
  item.supported = true;
  return osPathIsSafe(item.local_path.c_str());
}

bool prepareFont(Item& item) {
  std::string file = sanitizeComponent(item.name.c_str(), "font.ttf");
  std::string ext = extensionOf(file);
  if (ext.empty()) {
    file += ".ttf";
    ext = ".ttf";
  }
  if (ext != ".ttf" && ext != ".otf" && ext != ".cpfont") {
    item.state = ItemState::Unsupported;
    item.supported = false;
    return false;
  }
  const bool cpfont = ext == ".cpfont";
  const bool valid = cpfont ? FontInstaller::isValidCpfontFilename(file.c_str())
                            : FontInstaller::isValidVectorFontFilename(file.c_str());
  if (!valid) {
    file = cpfont ? "font.cpfont" : "font.ttf";
  }
  const std::string family = sanitizeFamily(item.display_name.empty() ? item.name.c_str() : item.display_name.c_str(),
                                             item.resource_id.c_str());
  FontInstaller installer(sdFontSystem.registry());
  if (!installer.ensureFamilyDir(family.c_str())) {
    item.state = ItemState::Unsupported;
    item.supported = false;
    return false;
  }
  char path[256] = {0};
  FontInstaller::buildFontPath(family.c_str(), file.c_str(), path, sizeof(path));
  if (!osPathIsSafe(path)) {
    item.state = ItemState::Unsupported;
    item.supported = false;
    return false;
  }
  item.local_path = path;
  item.supported = true;
  return true;
}

bool prepareItem(Item& item) {
  item.local_path.clear();
  item.supported = false;
  item.state = ItemState::Pending;
  if (item.task_id.empty() || item.name.empty() || item.download_url.empty()) return false;
  switch (item.type) {
    case Type::Book:
      return prepareBook(item);
    case Type::Badge:
      return prepareBadge(item);
    case Type::Font:
      return prepareFont(item);
    default:
      item.state = ItemState::Unsupported;
      return false;
  }
}

std::string hashFile(const char* path, bool& ok) {
  ok = false;
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return {};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buffer[4096];
  while (true) {
    const int n = file.read(buffer, sizeof(buffer));
    if (n < 0) {
      mbedtls_sha256_free(&ctx);
      file.close();
      return {};
    }
    if (n == 0) break;
    mbedtls_sha256_update(&ctx, buffer, static_cast<size_t>(n));
  }
  file.close();
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  char hex[65];
  for (size_t i = 0; i < sizeof(digest); ++i) {
    std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  hex[64] = '\0';
  ok = true;
  return hex;
}

bool fileMatches(const Item& item) {
  if (item.local_path.empty() || !Storage.exists(item.local_path.c_str())) return false;
  HalFile file = Storage.open(item.local_path.c_str(), O_RDONLY);
  if (!file) return false;
  const uint64_t size = file.fileSize64();
  file.close();
  if (item.file_size != 0 && size != item.file_size) return false;
  if (item.sha256.empty()) return false;
  bool ok = false;
  const std::string actual = hashFile(item.local_path.c_str(), ok);
  return ok && actual == lower(item.sha256.c_str());
}

bool ackItem(const Item& item, std::string& error) {
  if (item.task_id.empty()) {
    error = "missing deliveryId";
    return false;
  }
  if (!ensureAccess()) {
    error = "device token is missing or expired";
    return false;
  }
  const std::string path = std::string("/deliveries/") + item.task_id.c_str() + "/acknowledgements";
  auto post = [&]() { return httpRequest("POST", urlFor(path.c_str()), "", g_tokens.access.c_str(), 1024); };
  HttpResult r = post();
  if (r.status == 401 && refreshAccess()) r = post();
  if (r.status < 200 || r.status >= 300) {
    if (r.status == 401) clearTokens();
    error = "acknowledgement HTTP " + std::to_string(r.status);
    return false;
  }
  LOG_INF("RES", "acknowledged deliveryId=%s", item.task_id.c_str());
  return true;
}

std::string listError(const std::string& body, int status) {
  if (body.empty()) return status > 0 ? ("HTTP " + std::to_string(status)) : "no response";
  JsonDocument response;
  if (deserializeJson(response, body) != DeserializationError::Ok) {
    return "HTTP " + std::to_string(status);
  }
  return jsonError(response, "request rejected");
}

}  // namespace

bool configured() { return true; }
const char* keySource() { return "partner"; }
const char* keyValue() { return ""; }
const char* deviceId() { return plus_ota::deviceId(); }
const char* currentVersion() { return plus_ota::currentVersion(); }

BindResult bindDevice() {
  BindResult result;
  loadTokens();
  if (accessValid()) {
    result.ok = true;
    result.already_bound = true;
    return result;
  }

  const int64_t now = epochNow();
  const bool pairing_live = !g_tokens.device_code.empty() &&
                            (g_tokens.pairing_exp == 0 || now == 0 || now < g_tokens.pairing_exp);
  if (pairing_live) {
    // The UI re-enters every few seconds; respect the service's poll interval
    // without a request, and keep showing the same activation code.
    const uint32_t wait_ms = static_cast<uint32_t>(std::max(1, g_tokens.poll_seconds)) * 1000u;
    if (g_tokens.last_poll_ms != 0 && millis() - g_tokens.last_poll_ms < wait_ms) {
      result.ok = true;
      result.code = g_tokens.activation_code;
      return result;
    }
    g_tokens.last_poll_ms = millis();
    JsonDocument req;
    req["deviceCode"] = g_tokens.device_code;
    std::string payload;
    serializeJson(req, payload);
    const HttpResult r = httpRequest("POST", urlFor(kTokenPath), payload, nullptr, 8192);
    LOG_INF("RES", "token poll: http=%d body=%u B", r.status, static_cast<unsigned>(r.body.size()));
    if (r.status == 200) {
      JsonDocument doc;
      if (deserializeJson(doc, r.body) == DeserializationError::Ok) {
        parseTokenResponse(doc);
        if (!g_tokens.access.empty()) {
          result.ok = true;
          LOG_INF("RES", "device token received");
          return result;
        }
      }
      result.error = "bad token response";
      return result;
    }
    if (r.status == 409) {
      result.ok = true;  // authorization_pending: keep the guide open
      result.code = g_tokens.activation_code;
      return result;
    }
    if (r.status == 429) {
      g_tokens.poll_seconds = std::min(60, std::max(5, g_tokens.poll_seconds + 5));
      saveTokens();
      result.ok = true;
      result.code = g_tokens.activation_code;
      return result;
    }
    if (r.status == 410) {
      g_tokens.device_code.clear();
      g_tokens.activation_code.clear();
      g_tokens.pairing_exp = 0;
      saveTokens();
      // Fall through to a fresh pairing request.
    } else {
      result.http = r.status;
      result.error = listError(r.body, r.status);
      return result;
    }
  }

  JsonDocument req;
  req["integrationId"] = plus_cloudzao::kIntegrationId;
  req["deviceMac"] = plus_ota::deviceId();
  req["currentVersion"] = plus_ota::currentVersion();
  std::string payload;
  serializeJson(req, payload);
  const HttpResult r = httpRequest("POST", urlFor(kPairPath), payload, nullptr, 8192);
  result.http = r.status;
  LOG_INF("RES", "pairing: http=%d body=%u B", r.status, static_cast<unsigned>(r.body.size()));
  if (r.status != 200) {
    result.error = listError(r.body, r.status);
    if (r.status == 409) result.error = "设备已绑定，或暂时无法生成验证码；请在小程序中检查绑定";
    return result;
  }
  JsonDocument doc;
  if (deserializeJson(doc, r.body) != DeserializationError::Ok) {
    result.error = "bad JSON from the pairing service";
    return result;
  }
  const char* code = doc["activationCode"] | "";
  const char* device = doc["deviceCode"] | "";
  if (code[0] == '\0' || device[0] == '\0') {
    result.error = "pairing response is missing the activation code";
    return result;
  }
  g_tokens.activation_code = code;
  g_tokens.device_code = device;
  const int expires = doc["expiresIn"] | 0;
  const int poll = doc["pollInterval"] | 5;
  g_tokens.pairing_exp = (now != 0 && expires > 0) ? now + expires : 0;
  g_tokens.poll_seconds = std::max(1, poll);
  g_tokens.last_poll_ms = millis();
  saveTokens();
  result.ok = true;
  result.code = code;
  LOG_INF("RES", "activation code received (%u chars)", static_cast<unsigned>(result.code.size()));
  return result;
}

FetchResult fetchResources() {
  FetchResult result;
  if (!ensureAccess()) {
    result.http = 401;
    result.business_code = 401;
    result.error = "设备未绑定或登录已过期";
    return result;
  }

  auto get = [&]() { return httpRequest("GET", urlFor(kListPath), "", g_tokens.access.c_str()); };
  HttpResult r = get();
  if (r.status == 401 && refreshAccess()) r = get();
  result.http = r.status;
  LOG_INF("RES", "deliveries: http=%d body=%u B", r.status, static_cast<unsigned>(r.body.size()));
  if (r.status == 401) {
    clearTokens();
    result.business_code = 401;
    result.error = "设备未绑定或登录已过期，请在小程序中重新绑定";
    return result;
  }
  if (r.status < 200 || r.status >= 300) {
    result.error = listError(r.body, r.status);
    return result;
  }

  JsonDocument response;
  if (deserializeJson(response, r.body) != DeserializationError::Ok) {
    result.error = "bad JSON from the delivery service";
    return result;
  }
  result.business_code = 0;
  const JsonVariantConst data = response["items"];
  if (data.isNull()) {
    result.ok = true;
    return result;
  }
  if (!data.is<JsonArrayConst>()) {
    result.error = "delivery items is not an array";
    return result;
  }

  Items items;
  for (const JsonVariantConst object : data.as<JsonArrayConst>()) {
    if (!object.is<JsonObjectConst>()) continue;
    if (items.size() >= kMaxResources) break;
    Item item;
    const char* kind = object["kind"] | "";
    const char* delivery = object["deliveryId"] | "";
    const char* name = object["name"] | "";
    const char* url = object["downloadUrl"] | "";
    if (kind[0] == '\0' || delivery[0] == '\0' || name[0] == '\0' || url[0] == '\0') continue;
    if (strcasecmp(kind, "book") == 0) {
      item.type = Type::Book;
    } else if (strcasecmp(kind, "badge") == 0) {
      item.type = Type::Badge;
    } else if (strcasecmp(kind, "font") == 0) {
      item.type = Type::Font;
    } else {
      continue;
    }
    item.task_id = delivery;
    item.resource_id = delivery;
    item.name = name;
    const char* title = object["title"] | "";
    item.display_name = title[0] != '\0' ? title : name;
    item.content_type = object["mediaType"] | "";
    item.community_type = object["imageUsage"] | "";
    item.download_url = url;
    item.format = object["format"] | "";
    item.sha256 = object["sha256"] | "";
    parseU64(object["sizeBytes"], item.file_size);
    prepareItem(item);
    items.push_back(std::move(item));
  }
  result.ok = true;
  result.items = std::move(items);
  LOG_INF("RES", "deliveries parsed: %u item(s)", static_cast<unsigned>(result.items.size()));
  return result;
}

TransferResult downloadAndAck(Item& item, const ProgressFn progress, void* ctx, const bool* cancel_flag) {
  TransferResult result;
  if (!item.supported || item.local_path.empty()) {
    if (!prepareItem(item)) {
      result.unsupported = true;
      result.error = "unsupported resource format";
      return result;
    }
  }
  if (item.sha256.empty()) {
    result.error = "the service did not provide sha256";
    return result;
  }

  if (!fileMatches(item)) {
    const std::string tmp = std::string(item.local_path.c_str()) + ".part";
    Storage.remove(tmp.c_str());
    HalFile file;
    if (!Storage.openFileForWrite("RES", tmp.c_str(), file)) {
      result.error = "cannot write the SD card";
      return result;
    }

    freeink::SecureHttpClient http;
    http.setInsecure();
    http.setTimeout(30000);
    http.setFollowRedirects(3);
    http.setAllowRedirectDowngrade(false);
    http.setReuse(false);
    http.setReserveFullRecord(true);
    http.setUserAgent("CPYOS-Plus/" + std::string(plus_ota::currentVersion()));
    if (!http.begin(item.download_url.c_str())) {
      file.close();
      Storage.remove(tmp.c_str());
      result.error = "invalid download URL";
      return result;
    }
    bool write_failed = false;
    uint64_t received = 0;
    const auto sink = [&](const uint8_t* data, const size_t len) {
      if (cancel_flag != nullptr && *cancel_flag) return false;
      if (file.write(data, len) != len) {
        write_failed = true;
        return false;
      }
      received += len;
      if (progress != nullptr) progress(static_cast<size_t>(received), static_cast<size_t>(item.file_size), ctx);
      return true;
    };
    const auto abort = [cancel_flag]() { return cancel_flag != nullptr && *cancel_flag; };
    const int status = http.GET(sink, abort);
    const bool complete = http.responseComplete();
    http.end();
    const bool close_ok = file.close();
    if (cancel_flag != nullptr && *cancel_flag) {
      Storage.remove(tmp.c_str());
      result.error = "cancelled";
      return result;
    }
    if (write_failed || !close_ok) {
      Storage.remove(tmp.c_str());
      result.error = "cannot write the SD card";
      return result;
    }
    if (status != 200) {
      Storage.remove(tmp.c_str());
      result.error = "download HTTP " + std::to_string(status);
      return result;
    }
    if (!complete) {
      Storage.remove(tmp.c_str());
      result.error = "incomplete download";
      return result;
    }
    if (item.file_size != 0 && received != item.file_size) {
      Storage.remove(tmp.c_str());
      result.error = "size mismatch";
      return result;
    }
    bool hash_ok = false;
    const std::string actual = hashFile(tmp.c_str(), hash_ok);
    if (!hash_ok || actual != lower(item.sha256.c_str())) {
      Storage.remove(tmp.c_str());
      result.error = "sha256 mismatch";
      return result;
    }
    if (!Storage.replaceFile(tmp.c_str(), item.local_path.c_str())) {
      Storage.remove(tmp.c_str());
      result.error = "cannot install the downloaded file";
      return result;
    }
    LOG_INF("RES", "saved taskId=%s -> %s (%llu B)", item.task_id.c_str(), item.local_path.c_str(),
            static_cast<unsigned long long>(received));
  } else {
    LOG_INF("RES", "reusing verified file for taskId=%s", item.task_id.c_str());
  }

  result.ok = true;
  std::string ack_error;
  if (!ackItem(item, ack_error)) {
    item.state = ItemState::SavedNotAcknowledged;
    result.error = ack_error;
    LOG_ERR("RES", "saved but acknowledgement failed: %s", ack_error.c_str());
    return result;
  }
  item.state = ItemState::Pending;
  result.acked = true;
  return result;
}

std::string formatBytes(const uint64_t bytes) {
  char buf[40];
  if (bytes < 1024) {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  } else if (bytes < 1024ull * 1024ull) {
    std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  }
  return buf;
}

const char* typeName(const Type type) {
  switch (type) {
    case Type::Book:
      return "BOOK";
    case Type::Badge:
      return "BADGE";
    case Type::Font:
      return "FONT";
    default:
      return "UNKNOWN";
  }
}

std::string extensionOf(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return {};
  return lower(name.substr(dot));
}

}  // namespace plus_resources
