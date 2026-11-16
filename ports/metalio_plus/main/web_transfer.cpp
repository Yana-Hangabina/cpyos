// Minimal SD file transfer over HTTP for the Metalio Plus port.
//
// This is deliberately smaller than the CPYOS web server (which needs the
// Arduino ESPAsyncWebServer stack): it serves one self-contained page with
// list / download / upload / delete under the SD root, so a book or font can be
// put on the device without pulling the card.
//
// Safety:
//   * paths are logical ("/Books/x.epub"), resolved under /sdcard;
//   * ".." is rejected and the dot directory /.crosspoint (settings, cookies,
//     device secret) is inaccessible in both directions;
//   * uploads stream to "<path>.part" and are renamed only on success;
//   * there is no authentication: run it on the SoftAP you just created, not on
//     an untrusted network. The console commands `web` / `weboff` gate it.
//
// UNTESTED ON HARDWARE.

#include "web_transfer.h"
#include "web_pages.inc"
#include "font_store.h"
#include "sleep_wake.h"
#include <SdCardFontSystem.h>
#ifndef PROGMEM  // the generated asset header uses the Arduino qualifier
#define PROGMEM
#endif
#include <network/html/js/jszip_minJs.generated.h>

#include <PinyinDb.h>
#include <Epub/hyphenation/HyphenationStore.h>
#include <metalio_touch.h>
#include <cpyos_sd.h>
#include "usb_msc.h"

#include "bmp_export.h"
#include "form_fields.h"
#include "multipart.h"
#include "web_plugins.h"
#include "web_stores.h"
#include "webdav.h"
#include "HalDisplay.h"
#include "transfer_paths.h"
#include "ws_protocol.h"
#include "ws_upload.h"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <WiFi.h>
#include <DeviceSecret.h>
#include <HalFrontlight.h>
#include <plus_battery.h>
#include <plus_charger.h>
#include <CrossPointSettings.h>
// The shared settings list is header-only and already compiled into the UI
// component, so the POST handler below walks exactly the same rows the device
// Settings screen and the CPYOS web server do.
#include <SettingsList.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <esp_heap_caps.h>

namespace web_transfer {
namespace {

constexpr const char* TAG = "PLUS-WEB";
constexpr size_t kChunk = 4096;
constexpr uint64_t kMaxUpload = 512ull * 1024 * 1024;

httpd_handle_t s_server = nullptr;

httpd_uri_t makeUri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*),
                    void* context = nullptr) {
    httpd_uri_t entry{};
    entry.uri = uri;
    entry.method = method;
    entry.handler = handler;
    entry.user_ctx = context;
    return entry;
}

const char kPage[] = R"HTML(<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CPYOS Plus files</title>
<style>
body{font:16px system-ui,sans-serif;margin:0;padding:16px;background:#f5f5f3;color:#111}
h1{font-size:20px;margin:0 0 12px}
#path{font-family:ui-monospace,monospace;margin-bottom:12px;word-break:break-all}
ul{list-style:none;padding:0;margin:0 0 16px}
li{display:flex;gap:8px;align-items:center;padding:8px;border-bottom:1px solid #ddd}
li a{flex:1;color:#0645ad;text-decoration:none;word-break:break-all}
li .sz{color:#666;font-size:13px;white-space:nowrap}
button{border:1px solid #999;background:#fff;border-radius:6px;padding:4px 8px}
#msg{color:#555;font-size:14px;margin-top:8px}
</style>
<h1>CPYOS Plus — SD files</h1>
<p style="font-size:14px;margin:0 0 12px"><a href="/files/zh">官方文件管理（中文）</a> ·
<a href="/files/en">File manager (EN)</a> · <a href="/fonts">字体 / fonts</a> · <a href="/diag">诊断 / diagnostics</a></p>
<div id="path">/</div>
<ul id="list"></ul>
<input id="file" type="file" multiple>
<button onclick="up()">Upload</button>
<div id="msg"></div>
<script>
let cur='/';
const esc=s=>s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function fmt(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB'}
async function load(p){
  const r=await fetch('/api/list?path='+encodeURIComponent(p));
  if(!r.ok){msg('List failed: '+r.status);return}
  const j=await r.json(); cur=j.path; path.textContent=cur;
  const ul=list; ul.innerHTML='';
  if(cur!=='/'){const li=document.createElement('li');li.innerHTML='<a href="#" onclick="load(parent());return false">..</a>';ul.append(li)}
  for(const e of j.entries){
    const li=document.createElement('li');
    const name=esc(e.n)+(e.d?'/':'');
    const a=document.createElement('a');
    a.textContent=name;
    if(e.d){a.href='#';a.onclick=()=>{load(join(cur,e.n));return false}}
    else{a.href='/api/download?path='+encodeURIComponent(join(cur,e.n));a.download=e.n}
    li.append(a);
    const sz=document.createElement('span');sz.className='sz';sz.textContent=e.d?'dir':fmt(e.s);li.append(sz);
    const del=document.createElement('button');del.textContent='Delete';
    del.onclick=async()=>{if(!confirm('Delete '+name+'?'))return;
      const r=await fetch('/api/file?path='+encodeURIComponent(join(cur,e.n)),{method:'DELETE'});
      msg(r.ok?'Deleted':'Delete failed: '+r.status);load(cur);};
    li.append(del);ul.append(li);
  }
}
function join(a,b){return (a.endsWith('/')?a:a+'/')+b}
function parent(){const p=cur.replace(/\/$/,'');const i=p.lastIndexOf('/');return i<=0?'/':p.slice(0,i)}
function msg(t){document.getElementById('msg').textContent=t}
async function up(){
  for(const f of document.getElementById('file').files){
    msg('Uploading '+f.name+'…');
    const r=await fetch('/api/upload?path='+encodeURIComponent(join(cur,f.name)),{method:'POST',body:f});
    if(!r.ok){msg('Upload failed: '+r.status);return}
  }
  document.getElementById('file').value='';msg('Upload complete');load(cur);
}
load('/');
</script>)HTML";

std::string queryParam(httpd_req_t* req, const char* key) {
    char buffer[512] = {};
    if (httpd_req_get_url_query_str(req, buffer, sizeof(buffer)) != ESP_OK) return {};
    char value[384] = {};
    if (httpd_query_key_value(buffer, key, value, sizeof(value)) != ESP_OK) return {};
    return paths::urlDecode(value);
}

void jsonEscape(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned char>(c));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
}

esp_err_t pageHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t listHandler(httpd_req_t* req) {
    const std::string logical = queryParam(req, "path").empty() ? "/" : queryParam(req, "path");
    if (!paths::safe(logical)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    DIR* dir = opendir(paths::toReal(logical).c_str());
    if (dir == nullptr) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    std::string body = "{\"path\":\"";
    jsonEscape(logical, body);
    body += "\",\"entries\":[";
    bool first = true;
    while (struct dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::string real = paths::toReal(logical) + "/" + entry->d_name;
        struct stat st{};
        if (stat(real.c_str(), &st) != 0) continue;
        if (!first) body += ',';
        first = false;
        body += "{\"n\":\"";
        jsonEscape(entry->d_name, body);
        body += "\",\"d\":";
        body += S_ISDIR(st.st_mode) ? "1" : "0";
        body += ",\"s\":";
        body += std::to_string(static_cast<long long>(st.st_size));
        body += '}';
    }
    closedir(dir);
    body += "]}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t downloadHandler(httpd_req_t* req) {
    const std::string logical = queryParam(req, "path");
    if (!paths::safe(logical) || logical == "/") {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    int fd = open(paths::toReal(logical).c_str(), O_RDONLY);
    if (fd < 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    std::string name = logical.substr(logical.find_last_of('/') + 1);
    std::string disposition = "attachment; filename=\"" + name + "\"";
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
    std::vector<uint8_t> buffer(kChunk);
    ssize_t got = 0;
    esp_err_t err = ESP_OK;
    while ((got = read(fd, buffer.data(), buffer.size())) > 0) {
        err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buffer.data()), got);
        if (err != ESP_OK) break;
    }
    close(fd);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

esp_err_t uploadHandler(httpd_req_t* req) {
    const std::string logical = queryParam(req, "path");
    if (!paths::safe(logical) || logical == "/") {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (req->content_len <= 0 || static_cast<uint64_t>(req->content_len) > kMaxUpload) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }
    const std::string real = paths::toReal(logical);
    const std::string temp = real + ".part";
    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create");
    }
    std::vector<char> buffer(kChunk);
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        const int want = std::min<int>(remaining, static_cast<int>(buffer.size()));
        const int got = httpd_req_recv(req, buffer.data(), want);
        if (got <= 0) {
            ok = false;
            break;
        }
        if (write(fd, buffer.data(), got) != got) {
            ok = false;
            break;
        }
        remaining -= got;
    }
    close(fd);
    if (!ok) {
        unlink(temp.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }
    unlink(real.c_str());
    if (rename(temp.c_str(), real.c_str()) != 0) {
        unlink(temp.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    ESP_LOGI(TAG, "uploaded %s (%d bytes)", logical.c_str(), req->content_len);
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, "ok", 2);
}

// GET /api/screenshot.bmp — the current 1-bit framebuffer as a top-down BMP,
// so a bring-up session can see the screen from a browser without a camera.
esp_err_t screenshotHandler(httpd_req_t* req) {
    const uint8_t* frame = HalDisplay::frameBufferSnapshot();
    if (frame == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no framebuffer");
    }
    const int width = HalDisplay::DISPLAY_WIDTH;
    const int height = HalDisplay::DISPLAY_HEIGHT;
    const size_t size = bmp_export::fileSize(width, height);
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) buffer = static_cast<uint8_t*>(std::malloc(size));
    if (buffer == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    const bool ok = bmp_export::writeMonoBmpTo(buffer, size, frame, HalDisplay::DISPLAY_WIDTH_BYTES,
                                               width, height, HalDisplay::frameBufferInverted());
    if (!ok) {
        std::free(buffer);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=screen.bmp");
    const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(buffer), size);
    std::free(buffer);
    return err;
}

// GET /api/settings — a compact view of the settings most worth inspecting
// remotely. POST /api/settings (below) applies changes; this GET stays as-is so
// the existing settings page keeps working.
esp_err_t settingsHandler(httpd_req_t* req) {
    char body[320];
    snprintf(body, sizeof(body),
             "{\"language\":%u,\"uiTheme\":%u,\"uiDensity\":%u,\"fontPointSize\":%u,"
             "\"orientation\":%u,\"sleepTimeoutMinutes\":%u,\"frontlightOn\":%u,"
             "\"frontlightBrightness\":%u,\"frontlightWarmth\":%u}",
             static_cast<unsigned>(SETTINGS.language), static_cast<unsigned>(SETTINGS.uiTheme),
             static_cast<unsigned>(SETTINGS.uiDensity), static_cast<unsigned>(SETTINGS.fontPointSize),
             static_cast<unsigned>(SETTINGS.orientation),
             static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes),
             static_cast<unsigned>(SETTINGS.frontlightOn),
             static_cast<unsigned>(SETTINGS.frontlightBrightness),
             static_cast<unsigned>(SETTINGS.frontlightWarmth));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// POST /api/settings — the esp_http_server twin of
// CrossPointWebServer::handlePostSettings(): a flat JSON object {key: value} is
// clamped against the shared settings list, persisted with saveToFile() and the
// frontlight is applied live.
//
// Two deliberate differences from a blind copy:
//   * credentials are never writable (KOReader/OPDS passwords, Wi-Fi secrets),
//     and anything whose key looks like a secret is refused, so a future
//     credential row cannot be opened by accident;
//   * unknown keys and out-of-range values are reported instead of silently
//     ignored, and the whole batch is validated before the first write so a
//     typo never half-applies.
// Values are never logged — only key names, and only when rejected.
constexpr size_t kMaxSettingsBody = 8192;

esp_err_t sendSettingsJson(httpd_req_t* req, const char* status, const char* body) {
    if (status != nullptr) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

bool containsIgnoreCase(const char* haystack, const char* needle) {
    const size_t needleLen = std::strlen(needle);
    for (const char* p = haystack; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < needleLen && p[i] != '\0' &&
               std::tolower(static_cast<unsigned char>(p[i])) == needle[i]) {
            ++i;
        }
        if (i == needleLen) return true;
    }
    return false;
}

// Credentials and anything that reads like one stay device-only.
bool isSecretSetting(const SettingInfo& s) {
    if (s.key == nullptr) return true;
    if (s.obfuscated || s.nameId == StrId::STR_KOREADER_PASSWORD) return true;
    static const char* kSecretWords[] = {"password", "passwd", "secret", "token", "credential", "wifipass",
                                         "wifissid"};
    for (const char* word : kSecretWords) {
        if (containsIgnoreCase(s.key, word)) return true;
    }
    return false;
}

// nullptr = acceptable, otherwise the reason reported back to the client.
const char* settingValueProblem(const SettingInfo& s, JsonVariantConst value) {
    switch (s.type) {
        case SettingType::TOGGLE:
            return (value.is<int>() || value.is<bool>()) ? nullptr : "bad value";
        case SettingType::ENUM: {
            if (!value.is<int>()) return "bad value";
            const int maxValue = s.enumStringValues.empty()
                                     ? static_cast<int>(s.enumLabels().size())
                                     : static_cast<int>(s.enumStringValues.size());
            const int parsed = value.as<int>();
            return (parsed >= 0 && parsed < maxValue) ? nullptr : "bad value";
        }
        case SettingType::VALUE: {
            if (!value.is<int>()) return "bad value";
            const int parsed = value.as<int>();
            return (parsed >= s.valueRange.min && parsed <= s.valueRange.max) ? nullptr : "bad value";
        }
        case SettingType::STRING: {
            if (!value.is<const char*>()) return "bad value";
            const char* text = value.as<const char*>();
            const size_t length = text != nullptr ? std::strlen(text) : 0;
            const size_t limit = s.stringMaxLen > 0 ? s.stringMaxLen - 1 : 256;
            return length <= limit ? nullptr : "bad value";
        }
        default:
            return "unsupported";
    }
}

void applySettingValue(const SettingInfo& s, JsonVariantConst value, int& applied) {
    switch (s.type) {
        case SettingType::TOGGLE: {
            const int parsed = value.as<int>() ? 1 : 0;
            if (s.valuePtr) SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(parsed);
            applied++;
            break;
        }
        case SettingType::ENUM: {
            const int parsed = value.as<int>();
            if (s.valuePtr) {
                SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(parsed);
            } else if (s.valueSetter) {
                s.valueSetter(static_cast<uint8_t>(parsed));
            }
            applied++;
            break;
        }
        case SettingType::VALUE: {
            const int parsed = value.as<int>();
            if (s.valuePtr) {
                SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(parsed);
            } else if (s.signedValuePtr) {
                SETTINGS.*(s.signedValuePtr) = static_cast<int8_t>(parsed);
            }
            applied++;
            break;
        }
        case SettingType::STRING: {
            const char* text = value.as<const char*>();
            const std::string parsed = text != nullptr ? text : "";
            if (s.stringSetter) {
                s.stringSetter(parsed);
            } else if (s.stringMaxLen > 0) {
                char* target = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
                strncpy(target, parsed.c_str(), s.stringMaxLen - 1);
                target[s.stringMaxLen - 1] = '\0';
            }
            applied++;
            break;
        }
        default:
            break;
    }
}

esp_err_t settingsPostHandler(httpd_req_t* req) {
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > kMaxSettingsBody) {
        return sendSettingsJson(req, "400 Bad Request", "{\"ok\":false,\"error\":\"body size\"}");
    }
    std::string body(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int got = httpd_req_recv(req, &body[received], body.size() - received);
        if (got <= 0) {
            return sendSettingsJson(req, "408 Request Timeout", "{\"ok\":false,\"error\":\"short body\"}");
        }
        received += static_cast<size_t>(got);
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body.c_str(), body.size());
    if (err || !doc.is<JsonObject>()) {
        return sendSettingsJson(req, "400 Bad Request", "{\"ok\":false,\"error\":\"invalid json\"}");
    }

    // Validation pass: nothing is written until the whole batch is known good.
    for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
        const char* key = pair.key().c_str();
        bool known = false;
        bool secret = false;
        const char* problem = nullptr;
        forEachSettingsListEntry(&sdFontSystem.registry(), [&](const SettingInfo& s) {
            if (known || s.key == nullptr || std::strcmp(s.key, key) != 0) return;
            known = true;
            secret = isSecretSetting(s);
            if (!secret) problem = settingValueProblem(s, pair.value());
        });
        if (!known) {
            ESP_LOGW(TAG, "settings POST: unknown key '%s'", key);
            return sendSettingsJson(req, "400 Bad Request", "{\"ok\":false,\"error\":\"unknown key\"}");
        }
        if (secret) {
            ESP_LOGW(TAG, "settings POST: refused credential key '%s'", key);
            return sendSettingsJson(req, "403 Forbidden", "{\"ok\":false,\"error\":\"not writable\"}");
        }
        if (problem != nullptr) {
            ESP_LOGW(TAG, "settings POST: rejected value for '%s'", key);
            char reply[96];
            snprintf(reply, sizeof(reply), "{\"ok\":false,\"error\":\"bad value\",\"key\":\"%s\"}", key);
            return sendSettingsJson(req, "400 Bad Request", reply);
        }
    }

    int applied = 0;
    for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
        const char* key = pair.key().c_str();
        forEachSettingsListEntry(&sdFontSystem.registry(), [&](const SettingInfo& s) {
            if (s.key == nullptr || std::strcmp(s.key, key) != 0) return;
            applySettingValue(s, pair.value(), applied);
        });
    }

    SETTINGS.saveToFile();
    Frontlight.setWarmth(SETTINGS.frontlightWarmth);
    Frontlight.setBrightness(SETTINGS.frontlightBrightness);
    ESP_LOGI(TAG, "settings POST applied %d setting(s)", applied);

    char reply[64];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"applied\":%d}", applied);
    return sendSettingsJson(req, nullptr, reply);
}

// GET /api/frontlight[?brightness=0..100&warmth=0..100&on=0|1]
// A bring-up control for the cool/warm PWM pair; it applies live and does not
// persist SETTINGS (the device Settings page owns persistence).
esp_err_t frontlightHandler(httpd_req_t* req) {
    bool ok = true;
    const auto percent = [&](const char* value, uint8_t& out) {
        if (value == nullptr) return;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || parsed < 0 || parsed > 100) {
            ok = false;
            return;
        }
        out = static_cast<uint8_t>(parsed);
    };
    const std::string brightness = queryParam(req, "brightness");
    const std::string warmth = queryParam(req, "warmth");
    const std::string on = queryParam(req, "on");
    uint8_t level = Frontlight.brightness();
    uint8_t warm = Frontlight.warmth();
    percent(brightness.empty() ? nullptr : brightness.c_str(), level);
    percent(warmth.empty() ? nullptr : warmth.c_str(), warm);
    if (brightness.empty() == false) Frontlight.setBrightness(level);
    if (warmth.empty() == false) Frontlight.setWarmth(warm);
    if (!on.empty()) Frontlight.setOn(on == "1");
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "0..100 required");
    char body[160];
    snprintf(body, sizeof(body),
             "{\"present\":%d,\"brightness\":%u,\"warmth\":%u,\"on\":%d}",
             Frontlight.present() ? 1 : 0, static_cast<unsigned>(Frontlight.brightness()),
             static_cast<unsigned>(Frontlight.warmth()), Frontlight.isOn() ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// Documented GET /api/status shape (docs/webserver-endpoints.md).
esp_err_t statusHandler(httpd_req_t* req) {
    const esp_app_desc_t* app = esp_app_get_description();
    const int mode = static_cast<int>(WiFi.getMode());
    const bool apOnly = (mode & static_cast<int>(WIFI_MODE_AP)) && !(mode & static_cast<int>(WIFI_MODE_STA));
    plus_battery_sample_t battery = {};
    const bool batteryOk = PlusBattery_Read(&battery);
    plus_charger_sample_t charger = {};
    const bool chargerOk = PlusCharger_Read(&charger);
    const bool charging = (batteryOk && battery.milliamps > 20) || (chargerOk && charger.vbus != 0);
    char core[640];
    snprintf(core, sizeof(core),
             "{\"version\":\"%s\",\"ip\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"freeHeap\":%u,"
             "\"uptime\":%llu,\"device\":\"cpyos_metalio_plus\",\"serial\":\"%s\",\"batteryValid\":%d,"
             "\"batteryMv\":%d,\"batterySoc\":%d,\"milliamps\":%d,\"charging\":%d",
             app ? app->version : "?", WiFi.localIP().toString().c_str(), apOnly ? "AP" : "STA",
             apOnly ? 0 : static_cast<int>(WiFi.RSSI()),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned long long>(esp_timer_get_time() / 1000000),
             WiFi.macAddress().c_str(),
             batteryOk ? 1 : 0, batteryOk ? battery.millivolts : 0,
             batteryOk ? battery.soc_percent : 0, batteryOk ? battery.milliamps : 0,
             charging ? 1 : 0);
    std::string body = core;
    // Documented optional identity: SHA-256(device secret || plugin), omitted
    // when the secret is unavailable.
    const std::string plugin = queryParam(req, "plugin");
    if (!plugin.empty() && plugin.size() <= 64) {
        uint8_t secret[32];
        if (deviceSecret(secret)) {
            uint8_t digest[32];
            mbedtls_sha256_context ctx;
            mbedtls_sha256_init(&ctx);
            mbedtls_sha256_starts(&ctx, 0);
            mbedtls_sha256_update(&ctx, secret, sizeof(secret));
            mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(plugin.data()), plugin.size());
            mbedtls_sha256_finish(&ctx, digest);
            mbedtls_sha256_free(&ctx);
            static const char* hex = "0123456789abcdef";
            std::string id;
            id.reserve(64);
            for (uint8_t byte : digest) {
                id += hex[byte >> 4];
                id += hex[byte & 0x0F];
            }
            body += ",\"deviceId\":\"" + id + "\"";
        }
    }
    body += '}';
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

// GET /api/diag — state the first hardware session can check from a phone.
// Read-only: it never triggers a data-pack load or touches I2C.
esp_err_t diagHandler(httpd_req_t* req) {
    const esp_app_desc_t* app = esp_app_get_description();
    const char* lang = hyphenation::HyphenationStore::shared().loadedLanguage();
    char langJson[8] = "null";
    if (lang != nullptr && std::isalnum(static_cast<unsigned char>(lang[0])) &&
        std::isalnum(static_cast<unsigned char>(lang[1]))) {
        snprintf(langJson, sizeof(langJson), "\"%c%c\"", lang[0], lang[1]);
    }
    struct statvfs sd = {};
    const bool sdOk = cpyos_sd_is_mounted() && statvfs("/sdcard", &sd) == 0;
    char body[768];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"project\":\"%s\",\"uptimeMs\":%llu,\"reset\":%d,\"wake\":%d,"
             "\"heapFree\":%u,\"heapLargest\":%u,\"psramFree\":%u,"
             "\"sdMounted\":%d,\"sdFreeKb\":%llu,\"sdTotalKb\":%llu,"
             "\"pinyinLoaded\":%d,\"hyphenLoaded\":%s,\"touch\":%d,\"usbMsc\":%d}",
             app ? app->version : "?", app ? app->project_name : "?",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000),
             static_cast<int>(esp_reset_reason()), static_cast<int>(plusFirstWakeupCause()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             sdOk ? 1 : 0,
             sdOk ? static_cast<unsigned long long>(sd.f_bavail) * sd.f_frsize / 1024 : 0ull,
             sdOk ? static_cast<unsigned long long>(sd.f_blocks) * sd.f_frsize / 1024 : 0ull,
             pinyin::PinyinDb::shared().loaded() ? 1 : 0, langJson, metalio_touch_ok() ? 1 : 0,
             usb_msc::running() ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// The official page loads JSZip for folder uploads. The generated header holds
// the gzip stream; serve it with Content-Encoding so the browser inflates it.
esp_err_t jszipHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, jszip_minJs, sizeof(jszip_minJs));
}

// Settings on the Plus are shared with the CPYOS web server: GET reports a
// compact view, POST /api/settings writes any non-credential row, and the
// frontlight keeps its own live control. Rows the API does not expose still
// live on the device.
esp_err_t settingsPageHandler(httpd_req_t* req) {
    static const char page[] = R"HTML(<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CPYOS Plus settings</title>
<style>body{font:15px system-ui,sans-serif;padding:16px;background:#f5f5f3;color:#111}
table{border-collapse:collapse;margin-bottom:16px}td{padding:4px 10px;border-bottom:1px solid #ddd}
td:first-child{color:#555}input[type=range]{width:220px;vertical-align:middle}
button{border:1px solid #999;background:#fff;border-radius:6px;padding:6px 10px}</style>
<h2>设备设置 / Device settings</h2>
<p style="font-size:13px;margin:0 0 12px">可用 <code>POST /api/settings</code> 写入同名字段（凭据类字段不开放）。
Exposed rows can be written with <code>POST /api/settings</code> (credentials excluded).</p>
<table id="t"></table>
<h3>前光 / Frontlight</h3>
<label>亮度 brightness <input id="b" type="range" min="0" max="100"></label><br><br>
<label>色温 warmth <input id="w" type="range" min="0" max="100"></label><br><br>
<button onclick="applyLight()">应用 / Apply</button><span id="st"></span>
<p>未列出的设置请在设备上修改。 Settings not listed here still live on the device.<br>
<a href="/">Home</a> · <a href="/files">文件 / Files</a> · <a href="/fonts">字体 / Fonts</a></p>
<script>
async function load(){
  const r=await fetch('/api/settings',{cache:'no-store'}); if(!r.ok) return;
  const d=await r.json();
  const rows=[['language',d.language],['theme',d.uiTheme],['density',d.uiDensity],
   ['font point size',d.fontPointSize],['orientation',d.orientation],
   ['sleep timeout (min)',d.sleepTimeoutMinutes],['frontlight on',d.frontlightOn],
   ['frontlight brightness',d.frontlightBrightness],['frontlight warmth',d.frontlightWarmth]];
  t.innerHTML=rows.map(x=>'<tr><td>'+x[0]+'</td><td>'+x[1]+'</td></tr>').join('');
  b.value=d.frontlightBrightness; w.value=d.frontlightWarmth;
}
async function applyLight(){
  st.textContent='…';
  const r=await fetch('/api/frontlight?brightness='+b.value+'&warmth='+w.value+'&on=1');
  st.textContent=r.ok?'OK':'failed '+r.status; if(r.ok) load();
}
load();
</script>)HTML";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// Pages are stored gzip-compressed (see scripts/gen_web_pages.py); browsers
// decompress them and the image keeps ~5x fewer bytes.
static esp_err_t sendGzPage(httpd_req_t* req, const unsigned char* data, unsigned int len) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, reinterpret_cast<const char*>(data), len);
}

// Official CPYOS home page (only /api/status is required). The minimal
// file page stays available at /simple for debugging.
esp_err_t homePageHandler(httpd_req_t* req) {
    const bool zh = std::strstr(req->uri, "/en") == nullptr;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return sendGzPage(req, zh ? kHomePageZh : kHomePageEn, zh ? kHomePageZhLen : kHomePageEnLen);
}

// Official CPYOS FilesPage served from flash. The page uses exactly the API
// implemented below (/api/files, /upload, /mkdir, /rename, /move, /delete), so
// no Arduino WebServer shim is needed.
esp_err_t filesPageHandler(httpd_req_t* req) {
    const bool zh = std::strstr(req->uri, "/en") == nullptr;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return sendGzPage(req, zh ? kFilesPageZh : kFilesPageEn, zh ? kFilesPageZhLen : kFilesPageEnLen);
}

// Official CPYOS FontsPage: list/upload/delete SD-card font families under
// /fonts/<family>/. The page sends the family as a text part before the file.
esp_err_t fontsPageHandler(httpd_req_t* req) {
    const bool zh = std::strstr(req->uri, "/en") == nullptr;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return sendGzPage(req, zh ? kFontsPageZh : kFontsPageEn, zh ? kFontsPageZhLen : kFontsPageEnLen);
}

esp_err_t fontsListHandler(httpd_req_t* req) {
    std::string body = "{\"families\":[";
    DIR* dir = opendir("/sdcard/fonts");
    size_t families = 0;
    if (dir != nullptr) {
        while (const dirent* entry = readdir(dir)) {
            const char* name = entry->d_name;
            if (name[0] == '.' || families >= 64) continue;
            const std::string path = std::string("/sdcard/fonts/") + name;
            struct stat st = {};
            if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            std::string files = "[";
            size_t count = 0;
            if (DIR* inner = opendir(path.c_str())) {
                while (const dirent* file = readdir(inner)) {
                    if (file->d_name[0] == '.' || count >= 64) continue;
                    const std::string fp = path + "/" + file->d_name;
                    struct stat fs = {};
                    if (stat(fp.c_str(), &fs) != 0 || S_ISDIR(fs.st_mode)) continue;
                    if (count++ > 0) files += ',';
                    files += "{\"size\":";
                    files += std::to_string(static_cast<long long>(fs.st_size));
                    files += '}';
                }
                closedir(inner);
            }
            files += ']';
            if (families++ > 0) body += ',';
            body += "{\"name\":\"";
            jsonEscape(name, body);
            body += "\",\"sizes\":[],\"files\":" + files + "}";
        }
        closedir(dir);
    }
    body += "]}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

bool removeTree(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) return false;
    bool ok = true;
    while (const dirent* entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        const std::string child = path + "/" + entry->d_name;
        struct stat st = {};
        if (stat(child.c_str(), &st) != 0) { ok = false; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (!removeTree(child)) ok = false;
        } else if (unlink(child.c_str()) != 0) {
            ok = false;
        }
    }
    closedir(dir);
    return rmdir(path.c_str()) == 0 && ok;
}

esp_err_t fontsDeleteHandler(httpd_req_t* req) {
    char body[256] = {};
    const int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body required");
    const char* key = std::strstr(body, "\"family\"");
    if (key == nullptr) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "family required");
    const char* first = std::strchr(key, '"');
    const char* second = first ? std::strchr(first + 1, '"') : nullptr;
    const char* third = second ? std::strchr(second + 1, '"') : nullptr;
    const char* fourth = third ? std::strchr(third + 1, '"') : nullptr;
    if (!first || !second || !third || !fourth) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "family required");
    }
    const std::string family(third + 1, fourth);
    if (!plus_fonts::safeFamily(family) || !removeTree("/sdcard/fonts/" + family)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cannot delete family");
    }
    sdFontSystem.markRegistryDirty();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

struct FontUpload {
    std::string family;
    std::string filename;
    std::string temp;
    std::string final;
    int fd = -1;
    bool completed = false;
};

bool fontField(void* ctx, const std::string& name, const std::string& value) {
    auto* up = static_cast<FontUpload*>(ctx);
    if (name != "family") return true;
    if (!plus_fonts::safeFamily(value)) return false;
    up->family = value;
    return true;
}

bool fontStart(void* ctx, const std::string& filename) {
    auto* up = static_cast<FontUpload*>(ctx);
    if (!plus_fonts::safeFamily(up->family) || !plus_fonts::safeFilename(filename)) return false;
    const std::string dir = "/sdcard/fonts/" + up->family;
    mkdir("/sdcard/fonts", 0777);
    mkdir(dir.c_str(), 0777);
    up->filename = filename;
    up->final = dir + "/" + filename;
    up->temp = up->final + ".part";
    up->fd = open(up->temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    return up->fd >= 0;
}

bool fontWrite(void* ctx, const uint8_t* data, size_t len) {
    auto* up = static_cast<FontUpload*>(ctx);
    return up->fd >= 0 && write(up->fd, data, len) == static_cast<ssize_t>(len);
}

void fontFinish(void* ctx) {
    auto* up = static_cast<FontUpload*>(ctx);
    if (up->fd < 0) return;
    close(up->fd);
    up->fd = -1;
    if (rename(up->temp.c_str(), up->final.c_str()) == 0) {
        up->completed = true;
    } else {
        unlink(up->temp.c_str());
    }
}

esp_err_t fontsUploadHandler(httpd_req_t* req) {
    char type[160] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "multipart required");
    }
    std::string boundary;
    if (!multipart::extractBoundary(type, boundary)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no boundary");
    }
    if (req->content_len <= 0 || static_cast<uint64_t>(req->content_len) > 32ull * 1024 * 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }
    FontUpload up;
    multipart::Sink sink{fontStart, fontWrite, fontFinish, &up, fontField};
    multipart::Parser parser(boundary, sink);
    std::vector<char> buffer(kChunk);
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        const int want = std::min<int>(remaining, static_cast<int>(buffer.size()));
        const int got = httpd_req_recv(req, buffer.data(), want);
        if (got <= 0) { ok = false; break; }
        if (!parser.feed(reinterpret_cast<const uint8_t*>(buffer.data()), got)) { ok = false; break; }
        remaining -= got;
    }
    if (ok && !parser.finish()) ok = false;
    if (!ok) {
        if (up.fd >= 0) close(up.fd);
        if (!up.temp.empty()) unlink(up.temp.c_str());
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload failed");
    }
    if (up.completed) sdFontSystem.markRegistryDirty();
    httpd_resp_set_type(req, "application/json");
    return up.completed ? httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN)
                        : httpd_resp_send(req, "{\"ok\":false,\"error\":\"cannot store file\"}",
                                          HTTPD_RESP_USE_STRLEN);
}

// Human-readable diagnostics page for a phone on the same network.
esp_err_t diagPageHandler(httpd_req_t* req) {
    static const char page[] = R"HTML(<!doctype html><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CPYOS Plus diagnostics</title>
<style>body{font:15px system-ui,sans-serif;padding:16px;background:#f5f5f3;color:#111}
table{border-collapse:collapse}td{padding:4px 10px;border-bottom:1px solid #ddd}
td:first-child{color:#555}</style>
<h2>CPYOS Plus diagnostics</h2>
<table id="t"></table>
<p><a href="/files/zh">文件管理</a> · <a href="/fonts">字体</a> · <a href="/api/screenshot.bmp">Screenshot (BMP)</a> · <a href="/">Home</a></p>
<script>
async function tick(){
  const r=await fetch('/api/diag',{cache:'no-store'}); if(!r.ok){document.title='diag '+r.status;return}
  const d=await r.json(); const rows=[
   ['version',d.version],['project',d.project],['uptime s',Math.round(d.uptimeMs/1000)],
   ['reset reason',d.reset],['wake cause',d.wake],
   ['internal free',d.heapFree],['internal largest',d.heapLargest],['PSRAM free',d.psramFree],
   ['SD mounted',d.sdMounted],['SD free KB',d.sdFreeKb],['SD total KB',d.sdTotalKb],
   ['pinyin loaded',d.pinyinLoaded],['hyphenation',d.hyphenLoaded],
   ['touch',d.touch],['USB MSC',d.usbMsc]];
  t.innerHTML=rows.map(r=>'<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>').join('');
}
tick(); setInterval(tick,2000);
</script>)HTML";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// Documented GET /api/files: [{name,size,isDirectory,isEpub}], dotfiles hidden.
esp_err_t filesHandler(httpd_req_t* req) {
    std::string logical = queryParam(req, "path");
    if (logical.empty()) logical = "/";
    if (!paths::safe(logical)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    DIR* dir = opendir(paths::toReal(logical).c_str());
    if (dir == nullptr) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    std::string body = "[";
    bool first = true;
    size_t count = 0;
    while (const dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (name[0] == '.' || strcmp(name, "System Volume Information") == 0 || strcmp(name, "XTCache") == 0) {
            continue;
        }
        if (++count > 1000) break;  // bounded response; the full UI uses batching
        std::string child = logical;
        if (child.empty() || child.back() != '/') child += '/';
        child += name;
        struct stat st = {};
        if (stat(paths::toReal(child).c_str(), &st) != 0) continue;
        const bool isDir = S_ISDIR(st.st_mode);
        const size_t len = strlen(name);
        const bool isEpub = !isDir && len >= 5 && strcasecmp(name + len - 5, ".epub") == 0;
        if (!first) body += ',';
        first = false;
        body += "{\"name\":\"";
        jsonEscape(name, body);
        body += "\",\"size\":";
        body += std::to_string(isDir ? 0 : static_cast<long long>(st.st_size));
        body += isDir ? ",\"isDirectory\":true,\"isEpub\":false}" : ",\"isDirectory\":false,\"isEpub\":";
        if (!isDir) body += isEpub ? "true}" : "false}";
    }
    closedir(dir);
    body += ']';
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

struct UploadCtx {
    std::string directory;
    std::string filename;
    std::string temp;
    std::string final;
    int fd = -1;
    bool started = false;
    bool completed = false;
};

bool uploadStart(void* ctx, const std::string& name) {
    auto* up = static_cast<UploadCtx*>(ctx);
    if (up->started) return false;  // one file per POST
    if (!ws_upload::safeFilename(name)) return false;
    std::string logical = up->directory;
    if (logical.empty() || logical.back() != '/') logical += '/';
    logical += name;
    if (!paths::safe(logical)) return false;
    up->started = true;
    up->filename = name;
    up->final = paths::toReal(logical);
    up->temp = up->final + ".part";
    up->fd = open(up->temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return up->fd >= 0;
}

bool uploadWrite(void* ctx, const uint8_t* data, size_t len) {
    auto* up = static_cast<UploadCtx*>(ctx);
    size_t offset = 0;
    while (offset < len) {
        const ssize_t written = write(up->fd, data + offset, len - offset);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

void uploadFinish(void* ctx) {
    auto* up = static_cast<UploadCtx*>(ctx);
    if (up->fd >= 0) close(up->fd);
    up->fd = -1;
    unlink(up->final.c_str());  // documented: overwrite an existing file
    up->completed = rename(up->temp.c_str(), up->final.c_str()) == 0;
}

// Documented POST /upload: multipart/form-data, `path` query = destination dir.
esp_err_t uploadMultipartHandler(httpd_req_t* req) {
    char type[160] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "multipart required");
    }
    std::string boundary;
    if (!multipart::extractBoundary(type, boundary)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no boundary");
    }
    std::string dir = queryParam(req, "path");
    if (dir.empty()) dir = "/";
    if (!paths::safe(dir)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    struct stat dirStat = {};
    if (stat(paths::toReal(dir).c_str(), &dirStat) != 0 || !S_ISDIR(dirStat.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    if (req->content_len <= 0 || static_cast<uint64_t>(req->content_len) > kMaxUpload) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }

    UploadCtx up;
    up.directory = dir;
    multipart::Sink sink{uploadStart, uploadWrite, uploadFinish, &up};
    multipart::Parser parser(boundary, sink);
    std::vector<char> buffer(kChunk);
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        const int want = std::min<int>(remaining, static_cast<int>(buffer.size()));
        const int got = httpd_req_recv(req, buffer.data(), want);
        if (got <= 0) {
            ok = false;
            break;
        }
        if (!parser.feed(reinterpret_cast<const uint8_t*>(buffer.data()), got)) {
            ok = false;
            break;
        }
        remaining -= got;
    }
    if (ok && !parser.finish()) ok = false;
    if (!ok) {
        if (up.fd >= 0) close(up.fd);
        if (!up.temp.empty()) unlink(up.temp.c_str());
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload failed");
    }
    if (!up.completed) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot store file");
    }
    std::string message = "File uploaded successfully: " + up.filename;
    ESP_LOGI(TAG, "%s (%d bytes)", message.c_str(), req->content_len);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, message.c_str(), message.size());
}

bool readFormBody(httpd_req_t* req, std::string& body) {
    if (req->content_len <= 0 || req->content_len > 8192) return false;
    body.resize(static_cast<size_t>(req->content_len));
    size_t got = 0;
    while (got < body.size()) {
        const int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

std::string parentOf(const std::string& logical) {
    const size_t slash = logical.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return logical.substr(0, slash);
}

std::string baseName(const std::string& logical) {
    const size_t slash = logical.find_last_of('/');
    return slash == std::string::npos ? logical : logical.substr(slash + 1);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir.back() != '/') return dir + "/" + name;
    return dir + name;
}

// Documented POST /mkdir (form: name, path default "/").
esp_err_t mkdirHandler(httpd_req_t* req) {
    std::string body;
    if (!readFormBody(req, body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    std::vector<form_fields::Field> fields;
    form_fields::parseForm(body, fields);
    const auto* name = form_fields::get(fields, "name");
    const auto* path = form_fields::get(fields, "path");
    if (name == nullptr || !ws_upload::safeFilename(*name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    const std::string dir = path ? *path : "/";
    if (!paths::safe(dir)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    struct stat dirStat = {};
    if (stat(paths::toReal(dir).c_str(), &dirStat) != 0 || !S_ISDIR(dirStat.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const std::string child = joinPath(dir, *name);
    if (mkdir(paths::toReal(child).c_str(), 0755) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
    }
    std::string message = "Folder created successfully: " + *name;
    return httpd_resp_send(req, message.c_str(), message.size());
}

// Documented POST /rename (form: path, name; files and folders, same folder).
esp_err_t renameHandler(httpd_req_t* req) {
    std::string body;
    if (!readFormBody(req, body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    std::vector<form_fields::Field> fields;
    form_fields::parseForm(body, fields);
    const auto* path = form_fields::get(fields, "path");
    const auto* name = form_fields::get(fields, "name");
    if (path == nullptr || name == nullptr || !paths::safe(*path) || *path == "/" ||
        !ws_upload::safeFilename(*name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string target = joinPath(parentOf(*path), *name);
    if (access(paths::toReal(*path).c_str(), F_OK) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    if (access(paths::toReal(target).c_str(), F_OK) == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target exists");
    }
    if (rename(paths::toReal(*path).c_str(), paths::toReal(target).c_str()) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    return httpd_resp_send(req, "Renamed successfully", HTTPD_RESP_USE_STRLEN);
}

// Documented POST /move (form: path, dest folder).
esp_err_t moveHandler(httpd_req_t* req) {
    std::string body;
    if (!readFormBody(req, body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    std::vector<form_fields::Field> fields;
    form_fields::parseForm(body, fields);
    const auto* path = form_fields::get(fields, "path");
    const auto* dest = form_fields::get(fields, "dest");
    if (path == nullptr || dest == nullptr || !paths::safe(*path) || *path == "/" || !paths::safe(*dest)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    struct stat destStat = {};
    if (stat(paths::toReal(*dest).c_str(), &destStat) != 0 || !S_ISDIR(destStat.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    struct stat srcStat = {};
    if (stat(paths::toReal(*path).c_str(), &srcStat) != 0 || S_ISDIR(srcStat.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "files only");
    }
    const std::string target = joinPath(*dest, baseName(*path));
    if (access(paths::toReal(target).c_str(), F_OK) == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target exists");
    }
    if (rename(paths::toReal(*path).c_str(), paths::toReal(target).c_str()) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "move failed");
    }
    return httpd_resp_send(req, "Moved successfully", HTTPD_RESP_USE_STRLEN);
}

// Documented POST /delete (form: path, or paths=["/a","/b"]). Empty folders only.
esp_err_t deleteHandler(httpd_req_t* req) {
    std::string body;
    if (!readFormBody(req, body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    std::vector<form_fields::Field> fields;
    form_fields::parseForm(body, fields);
    std::vector<std::string> targets;
    if (const auto* path = form_fields::get(fields, "path")) {
        targets.push_back(*path);
    } else if (const auto* pathsJson = form_fields::get(fields, "paths")) {
        if (!form_fields::parseJsonStringArray(*pathsJson, targets)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad paths");
        }
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path required");
    }
    if (targets.empty()) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path required");
    for (const std::string& target : targets) {
        if (!paths::safe(target) || target == "/") {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        }
        const std::string real = paths::toReal(target);
        struct stat st = {};
        if (stat(real.c_str(), &st) != 0) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        }
        const int rc = S_ISDIR(st.st_mode) ? rmdir(real.c_str()) : unlink(real.c_str());
        if (rc != 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "delete failed");
        }
    }
    std::string message = "Deleted " + std::to_string(targets.size()) + " item(s)";
    return httpd_resp_send(req, message.c_str(), message.size());
}

esp_err_t fileHandler(httpd_req_t* req) {
    if (req->method != HTTP_DELETE) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "delete only");
    }
    const std::string logical = queryParam(req, "path");
    if (!paths::safe(logical) || logical == "/") {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string real = paths::toReal(logical);
    struct stat st{};
    if (stat(real.c_str(), &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const int rc = S_ISDIR(st.st_mode) ? rmdir(real.c_str()) : unlink(real.c_str());
    if (rc != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }
    return httpd_resp_send(req, "ok", 2);
}

}  // namespace

bool start() {
    if (s_server != nullptr) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    // WITHOUT this the control socket takes server_port + 1 = 81, and the
    // Calibre websocket server (also on 81) then fails in httpd_start with
    // "httpd_start(81) failed". Keep the ctrl port out of the 80/81 pair.
    config.ctrl_port = 32768;
    // Two httpd instances share CONFIG_LWIP_MAX_SOCKETS (10): 5 here + ctrl,
    // 2 + ctrl for the Calibre websocket. Leaving both at the default 7 made
    // the second httpd_start fail.
    config.max_open_sockets = 5;
    // 31 file/settings routes + 12 browser-plugin routes + 11 WebDAV methods.
    // The plugins add a job queue, a streaming upload and two network helpers
    // (see web_plugins.h); a full table would refuse the last registrations.
    config.max_uri_handlers = 64;
    // WebDAV registers "/*" after the exact endpoints; wildcard matching lets
    // it see arbitrary file paths while exact URIs still win for their methods.
    config.uri_match_fn = httpd_uri_match_wildcard;
    // /api/relay and /api/fetch run a TLS transfer inline on this task (the
    // reference does the same on its web-server task). The transport keeps its
    // body buffer and std::function plumbing on the stack, so 8 KiB — enough
    // for the file pages — is not enough here.
    config.stack_size = 16384;
    config.lru_purge_enable = true;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = nullptr;
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    const httpd_uri_t page = makeUri("/simple", HTTP_GET, pageHandler);
    const httpd_uri_t home = makeUri("/", HTTP_GET, homePageHandler);
    const httpd_uri_t homeEn = makeUri("/home/en", HTTP_GET, homePageHandler);
    const httpd_uri_t list = makeUri("/api/list", HTTP_GET, listHandler);
    const httpd_uri_t download = makeUri("/api/download", HTTP_GET, downloadHandler);
    const httpd_uri_t upload = makeUri("/api/upload", HTTP_POST, uploadHandler);
    const httpd_uri_t file = makeUri("/api/file", HTTP_DELETE, fileHandler);
    // Documented CPYOS API used by the web UI and third-party clients.
    const httpd_uri_t status = makeUri("/api/status", HTTP_GET, statusHandler);
    const httpd_uri_t files = makeUri("/api/files", HTTP_GET, filesHandler);
    const httpd_uri_t downloadAlias = makeUri("/download", HTTP_GET, downloadHandler);
    const httpd_uri_t uploadForm = makeUri("/upload", HTTP_POST, uploadMultipartHandler);
    const httpd_uri_t mkdirUri = makeUri("/mkdir", HTTP_POST, mkdirHandler);
    const httpd_uri_t renameUri = makeUri("/rename", HTTP_POST, renameHandler);
    const httpd_uri_t moveUri = makeUri("/move", HTTP_POST, moveHandler);
    const httpd_uri_t deleteUri = makeUri("/delete", HTTP_POST, deleteHandler);
    const httpd_uri_t screenshotUri = makeUri("/api/screenshot.bmp", HTTP_GET, screenshotHandler);
    const httpd_uri_t frontlightUri = makeUri("/api/frontlight", HTTP_GET, frontlightHandler);
    const httpd_uri_t settingsUri = makeUri("/api/settings", HTTP_GET, settingsHandler);
    const httpd_uri_t settingsPostUri = makeUri("/api/settings", HTTP_POST, settingsPostHandler);
    const httpd_uri_t diagUri = makeUri("/api/diag", HTTP_GET, diagHandler);
    const httpd_uri_t filesZhUri = makeUri("/files/zh", HTTP_GET, filesPageHandler);
    const httpd_uri_t filesEnUri = makeUri("/files/en", HTTP_GET, filesPageHandler);
    const httpd_uri_t filesUri = makeUri("/files", HTTP_GET, filesPageHandler);
    const httpd_uri_t settingsPageUri = makeUri("/settings", HTTP_GET, settingsPageHandler);
    const httpd_uri_t fontsPageUri = makeUri("/fonts", HTTP_GET, fontsPageHandler);
    const httpd_uri_t fontsEnUri = makeUri("/fonts/en", HTTP_GET, fontsPageHandler);
    const httpd_uri_t fontsListUri = makeUri("/api/fonts", HTTP_GET, fontsListHandler);
    const httpd_uri_t fontsUploadUri = makeUri("/api/fonts/upload", HTTP_POST, fontsUploadHandler);
    const httpd_uri_t fontsDeleteUri = makeUri("/api/fonts/delete", HTTP_POST, fontsDeleteHandler);
    const httpd_uri_t jszipUri = makeUri("/js/jszip.min.js", HTTP_GET, jszipHandler);
    const httpd_uri_t diagPageUri = makeUri("/diag", HTTP_GET, diagPageHandler);
    httpd_register_uri_handler(s_server, &page);
    httpd_register_uri_handler(s_server, &home);
    httpd_register_uri_handler(s_server, &homeEn);
    httpd_register_uri_handler(s_server, &list);
    httpd_register_uri_handler(s_server, &download);
    httpd_register_uri_handler(s_server, &upload);
    httpd_register_uri_handler(s_server, &file);
    httpd_register_uri_handler(s_server, &status);
    httpd_register_uri_handler(s_server, &files);
    httpd_register_uri_handler(s_server, &downloadAlias);
    httpd_register_uri_handler(s_server, &uploadForm);
    httpd_register_uri_handler(s_server, &mkdirUri);
    httpd_register_uri_handler(s_server, &renameUri);
    httpd_register_uri_handler(s_server, &moveUri);
    httpd_register_uri_handler(s_server, &deleteUri);
    httpd_register_uri_handler(s_server, &screenshotUri);
    httpd_register_uri_handler(s_server, &frontlightUri);
    httpd_register_uri_handler(s_server, &settingsUri);
    httpd_register_uri_handler(s_server, &settingsPostUri);
    httpd_register_uri_handler(s_server, &diagUri);
    httpd_register_uri_handler(s_server, &filesZhUri);
    httpd_register_uri_handler(s_server, &filesEnUri);
    httpd_register_uri_handler(s_server, &filesUri);
    httpd_register_uri_handler(s_server, &settingsPageUri);
    httpd_register_uri_handler(s_server, &fontsPageUri);
    httpd_register_uri_handler(s_server, &fontsEnUri);
    httpd_register_uri_handler(s_server, &fontsListUri);
    httpd_register_uri_handler(s_server, &fontsUploadUri);
    httpd_register_uri_handler(s_server, &fontsDeleteUri);
    httpd_register_uri_handler(s_server, &jszipUri);
    httpd_register_uri_handler(s_server, &diagPageUri);
    // Browser-side plugins. Must come before WebDAV: its "/*" handler matches
    // every path, and the server serves the FIRST handler whose method
    // matches, so a wildcard registered earlier would shadow these exact URIs.
    if (!web_plugins::registerRoutes(s_server)) {
        ESP_LOGW(TAG, "browser plugin routes not registered");
    }
    if (!web_stores::registerRoutes(s_server)) {
        ESP_LOGW(TAG, "OPDS/Wi-Fi store routes not registered");
    }
    if (!webdav::registerHandlers(s_server)) {
        ESP_LOGW(TAG, "WebDAV handlers not registered");
    }
    // Calibre's plugin protocol uses the documented WebSocket upload on port 81.
    if (!ws_upload::start()) {
        ESP_LOGW(TAG, "websocket upload (port 81) unavailable; HTTP upload still works");
    }
    ESP_LOGI(TAG, "file transfer ready on port 80 (websocket 81)");
    return true;
}

void stop() {
    ws_upload::stop();
    if (s_server == nullptr) return;
    httpd_stop(s_server);
    s_server = nullptr;
}

bool running() { return s_server != nullptr; }

}  // namespace web_transfer
