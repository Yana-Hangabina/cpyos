// Browser-side plugins on the Plus web server (port of the Read Pico feature).
//
// Plugin logic never runs on the device. A plugin is a folder on the SD card
// holding plugin.js (plus optional manifest.json); the browser loads it from
// /plugin, and external systems enqueue {plugin, action, args} jobs that any
// page hosting the plugin claims and completes. The firmware's share of the
// contract is small and bounded:
//
//   * a 6-slot job pool (plugin/action 24 B, args/result 192 B, 10 min lease,
//     id+claim echo so a late completion cannot overwrite a re-claimed job);
//   * one streaming upload endpoint (256 KB, temp file + atomic replace);
//   * generic capabilities a page cannot do itself: an outbound TLS request
//     (/api/relay), a large download straight to SD (/api/fetch), wolfSSL's
//     crypto vocabulary (/api/crypto) and a device-wrapped book key
//     (/api/book-key).
//
// Threading: esp_http_server runs every handler in ONE task (httpd_start
// creates a single "httpd" thread, see httpd_main.c), so the static job pool
// and the upload scratch state need no lock. If that ever changes, guard both.
//
// Security posture matches the reference: the port's web server is already an
// unauthenticated LAN surface (documented in web_transfer.cpp), no cookie,
// token or password value is ever logged here, and every buffer is bounded.

#include "web_plugins.h"

#include <ArduinoJson.h>
#include <BookKey.h>
#include <HalStorage.h>
#include <ResumableFetch.h>
#include <SecureHttpClient.h>
#include <Util.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <utility>
#include <vector>

#include "PluginLocations.h"
#include "ProtectedPaths.h"
#include "multipart.h"
#include "plugin_job_table.h"
#include "transfer_paths.h"

namespace web_plugins {
namespace {

constexpr const char* TAG = "PLUS-PLUGIN";
constexpr size_t kChunk = 2048;

// ---------------------------------------------------------------------------
// Small helpers (the shared ones in web_transfer.cpp live in an anonymous
// namespace there, so the few that are needed here are duplicated).
// ---------------------------------------------------------------------------

std::string queryParam(httpd_req_t* req, const char* key) {
    char buffer[1024] = {};
    if (httpd_req_get_url_query_str(req, buffer, sizeof(buffer)) != ESP_OK) return {};
    char value[768] = {};
    if (httpd_query_key_value(buffer, key, value, sizeof(value)) != ESP_OK) return {};
    return web_transfer::paths::urlDecode(value);
}

void jsonEscape(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
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

esp_err_t sendJson(httpd_req_t* req, JsonDocument& doc, const char* status = nullptr) {
    String out;
    serializeJson(doc, out);
    if (status != nullptr) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out.c_str(), out.length());
}

esp_err_t sendJsonError(httpd_req_t* req, const char* status, const char* message) {
    JsonDocument doc;
    doc["error"] = message;
    return sendJson(req, doc, status);
}

bool readBody(httpd_req_t* req, std::string& body, size_t maxBytes) {
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > maxBytes) return false;
    body.assign(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int got = httpd_req_recv(req, &body[received], body.size() - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return false;
        received += static_cast<size_t>(got);
    }
    return true;
}

// Parses a JSON request body. On failure the error response has already been
// sent, so the caller just returns ESP_OK.
bool readJsonBody(httpd_req_t* req, JsonDocument& doc, size_t maxBytes = 8192) {
    std::string body;
    if (!readBody(req, body, maxBytes)) {
        sendJsonError(req, "400 Bad Request", "bad body size");
        return false;
    }
    if (deserializeJson(doc, body.data(), body.size()) != DeserializationError::Ok) {
        sendJsonError(req, "400 Bad Request", "bad json");
        return false;
    }
    return true;
}

// A plugin name is one path component: no separators, no parent refs.
bool safeComponent(const std::string& s) {
    if (s.empty() || s.size() > 96) return false;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    return true;
}

// A plugin-supplied path: absolute, no parent refs, and never one of the
// credential stores the web surface must not touch. Use the shared
// canonicalising rule (ProtectedPaths.h): a raw prefix check would let
// "//.crosspoint/wifi.json", "/./.crosspoint/wifi.json", trailing dots/spaces
// or the "CROSSP~1" 8.3 alias reach the same file.
bool isPluginPath(const std::string& path) {
    return protectedpaths::isPluginPath(path);
}

std::string parentOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return path.substr(0, slash);
}

const char* pluginContentType(const std::string& file) {
    auto ends = [&](const char* suffix) {
        const size_t len = strlen(suffix);
        return file.size() >= len && file.compare(file.size() - len, len, suffix) == 0;
    };
    if (ends(".js")) return "application/javascript";
    if (ends(".css")) return "text/css";
    if (ends(".html")) return "text/html";
    if (ends(".json")) return "application/json";
    if (ends(".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

// Reads an optional {"name": "value"} object into header pairs, bounded.
void readHeaders(JsonVariantConst node, std::vector<std::pair<std::string, std::string>>& out) {
    if (!node.is<JsonObjectConst>()) return;
    for (JsonPairConst kv : node.as<JsonObjectConst>()) {
        if (out.size() >= 8) break;
        const std::string name = kv.key().c_str();
        if (name.empty() || name.size() > 64) continue;
        std::string value;
        if (kv.value().is<const char*>()) {
            value = kv.value().as<const char*>();
        } else {
            String serialized;
            serializeJson(kv.value(), serialized);
            value = serialized.c_str();
        }
        if (value.size() > 512) continue;
        out.emplace_back(name, value);
    }
}

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// GET /api/plugins -> [{ "name", "dir", "title", "mount" }, ...]
// Only folders carrying plugin.js are listed (that is what the page loads);
// manifest.json optionally supplies the title and the mount point.
// ---------------------------------------------------------------------------

esp_err_t pluginListHandler(httpd_req_t* req) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& entry : PluginLocations::scanPlugins()) {
        if (!entry.hasPluginJs) continue;
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = entry.name;
        obj["dir"] = entry.dir;  // the plugin keeps its own files here
        obj["title"] = entry.name;
        obj["mount"] = "settings";
        std::string manifest;
        if (entry.hasManifest &&
            Storage.readFileToString(TAG, entry.dir + "/manifest.json", 64 * 1024, manifest)) {
            JsonDocument m;
            if (deserializeJson(m, manifest) == DeserializationError::Ok) {
                if (m["title"].is<const char*>()) obj["title"] = m["title"];
                if (m["mount"].is<const char*>()) obj["mount"] = m["mount"];
            }
        }
    }
    return sendJson(req, doc);
}

// ---------------------------------------------------------------------------
// GET /plugin?name=<plugin>&file=<file> -> the plugin's own file from SD.
// ---------------------------------------------------------------------------

esp_err_t pluginFileHandler(httpd_req_t* req) {
    const std::string name = queryParam(req, "name");
    const std::string file = queryParam(req, "file");
    if (!safeComponent(name) || !safeComponent(file)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad plugin path");
    }
    const std::string dir = PluginLocations::findPluginDir(name.c_str());
    if (dir.empty()) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    HalFile handle = Storage.open((dir + "/" + file).c_str(), O_RDONLY);
    if (!handle || !handle.isOpen() || handle.isDirectory()) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    httpd_resp_set_type(req, pluginContentType(file));
    char buffer[kChunk];
    for (;;) {
        const int got = handle.read(buffer, sizeof(buffer));
        if (got <= 0) break;
        if (httpd_resp_send_chunk(req, buffer, got) != ESP_OK) {
            handle.close();
            return ESP_FAIL;
        }
    }
    handle.close();
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---------------------------------------------------------------------------
// GET /plugins-run -> headless executor page (same contract as the reference
// RunnerPage.html + shared/PluginHost.js: the page hosts every SD plugin with
// its UI hidden so registerAction() handlers claim and run queued jobs).
// ---------------------------------------------------------------------------

const char kRunnerPage[] = R"HTML(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>CPYOS Plugin Runner</title>
  <style>
    body { font-family: sans-serif; margin: 2em; color: #333; }
    #plugin-container { display: none; }
    #log { font-family: ui-monospace, monospace; font-size: 13px; color: #666; }
  </style>
</head>
<body>
  <h3>Plugin runner active</h3>
  <p>Jobs enqueued via <code>/api/plugin-jobs</code> execute while this page is open.</p>
  <p id="log"></p>
  <div id="plugin-container"></div>
  <script>
  window.CrossPoint = window.CrossPoint || {
    _pending: null,
    registerPlugin(fn) { this._pending = fn; },
    _actions: {},
    _actionBusy: false,
    _actionTimer: null,
    _pendingCompletion: null,
    registerAction(pluginName, action, fn) {
      (this._actions[pluginName] = this._actions[pluginName] || {})[action] = fn;
      if (!this._actionTimer) this._actionTimer = setInterval(() => this._pollJobs(), 2500);
    },
    async _pollJobs() {
      if (this._actionBusy) return;
      this._actionBusy = true;
      try {
        if (this._pendingCompletion && !(await this._sendCompletion())) return;
        for (const plugin of Object.keys(this._actions)) {
          let job;
          try { job = await (await fetch('/api/plugin-jobs/claim?plugin=' + encodeURIComponent(plugin))).json(); }
          catch (e) { continue; }
          if (!job || !job.id) continue;
          const fn = this._actions[plugin][job.action];
          let ok = false;
          let result;
          if (!fn) {
            result = { error: 'unknown action: ' + job.action };
          } else {
            try { result = (await fn(job.args || {})) || {}; ok = true; }
            catch (e) { result = { error: String((e && e.message) || e) }; }
          }
          document.getElementById('log').textContent = plugin + '/' + job.action + (ok ? ' ok' : ' failed');
          this._pendingCompletion = { id: job.id, claim: job.claim, ok, result };
          if (!(await this._sendCompletion())) return;
        }
      } finally {
        this._actionBusy = false;
      }
    },
    async _sendCompletion() {
      try {
        await this._post('/api/plugin-jobs/complete', this._pendingCompletion);
      } catch (e) {
        if (!(e.status >= 400 && e.status < 500)) return false;
      }
      this._pendingCompletion = null;
      return true;
    },
    async _readResponse(r, path) {
      if (!r.ok) {
        let detail = '';
        try { detail = (await r.json()).error || ''; } catch (e) {}
        const err = new Error(path + ' ' + r.status + (detail ? ': ' + detail : ''));
        err.status = r.status;
        throw err;
      }
      return r.json();
    },
    async _post(path, body) {
      return this._readResponse(await fetch(path, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body)
      }), path);
    },
    async relay(plugin, method, url, headers, body) {
      const r = await fetch('/api/relay', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ plugin, method, url, headers: headers || {}, body: body || '' })
      });
      if (!r.ok) return this._readResponse(r, '/api/relay');
      const raw = r.headers.get('X-Relay-Headers') || '[]';
      const list = JSON.parse(new TextDecoder().decode(Uint8Array.from(raw, (c) => c.charCodeAt(0))));
      return { status: Number(r.headers.get('X-Relay-Status')), headers: list, body: await r.text() };
    },
    crypto(op, fields) { return this._post('/api/crypto', Object.assign({ op }, fields || {})); },
    async writeFile(plugin, path, dataB64) {
      const bytes = Uint8Array.from(atob(dataB64 || ''), (c) => c.charCodeAt(0));
      const url = '/api/plugin-fs?plugin=' + encodeURIComponent(plugin) +
        '&path=' + encodeURIComponent(path);
      const form = new FormData();
      form.append('file', new Blob([bytes]), 'data');
      const r = await fetch(url, { method: 'POST', body: form });
      return this._readResponse(r, '/api/plugin-fs');
    },
    async fetchToSd(plugin, url, dest, headers) {
      const maxBytes = 2 * 1024 * 1024;
      let offset = 0;
      for (;;) {
        const result = await this._post('/api/fetch', {
          plugin, url, dest, headers: headers || {}, offset, maxBytes
        });
        if (result.complete !== false) return result;
        if (!Number.isFinite(result.bytes) || result.bytes <= offset) {
          throw new Error('/api/fetch made no progress');
        }
        offset = result.bytes;
      }
    }
  };

  async function loadPlugins() {
    const host = document.getElementById('plugin-container');
    if (!host) return;
    let list;
    try { list = await (await fetch('/api/plugins')).json(); } catch (e) { return; }
    for (const p of list) {
      const container = document.createElement('div');
      container.className = 'card';
      container.id = 'plugin-' + p.name;
      host.appendChild(container);
      CrossPoint._pending = null;
      try {
        await new Promise((resolve, reject) => {
          const s = document.createElement('script');
          s.src = '/plugin?name=' + encodeURIComponent(p.name) + '&file=plugin.js';
          s.onload = resolve; s.onerror = () => reject(new Error('load ' + p.name));
          document.body.appendChild(s);
        });
        if (typeof CrossPoint._pending === 'function') {
          CrossPoint._pending(container, {
            name: p.name,
            dir: p.dir,
            relay: (m, u, h, b) => CrossPoint.relay(p.name, m, u, h, b),
            crypto: (op, f) => CrossPoint.crypto(op, f),
            writeFile: (path, d) => CrossPoint.writeFile(p.name, path, d),
            fetchToSd: (u, dest, h) => CrossPoint.fetchToSd(p.name, u, dest, h),
            registerAction: (a, fn) => CrossPoint.registerAction(p.name, a, fn),
            pluginFile: (f) => '/plugin?name=' + encodeURIComponent(p.name) + '&file=' + encodeURIComponent(f)
          });
        }
      } catch (e) {
        const h = document.createElement('h2');
        h.textContent = p.title || p.name;
        const msg = document.createElement('p');
        msg.style.color = '#c0392b';
        msg.textContent = 'Failed to load plugin.';
        container.replaceChildren(h, msg);
      }
    }
  }
  loadPlugins();
  </script>
</body>
</html>
)HTML";

esp_err_t pluginRunnerPageHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kRunnerPage, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Plugin job queue: {plugin, action, args} in, {id, state, result} out.
// The lifecycle (bounded slots, lease re-queue, claim echo) lives in
// plugin_job_table.h so the host test pins it without IDF.
// ---------------------------------------------------------------------------

constexpr uint32_t kJobLeaseMs = 10UL * 60 * 1000;
plugin_jobs::Table<6> g_jobs;

// POST /api/plugin-jobs {plugin, action, args?} -> {id}
esp_err_t pluginJobSubmitHandler(httpd_req_t* req) {
    JsonDocument reqDoc;
    if (!readJsonBody(req, reqDoc)) return ESP_OK;
    const String plugin = reqDoc["plugin"] | "";
    const String action = reqDoc["action"] | "";
    // The claim response embeds `action` without JSON escaping.
    const auto identifierSafe = [](const String& s) {
        for (const char c : s) {
            if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
        }
        return !s.isEmpty();
    };
    if (!safeComponent(plugin.c_str()) || !identifierSafe(action) ||
        plugin.length() >= plugin_jobs::kPluginLen || action.length() >= plugin_jobs::kActionLen ||
        (!reqDoc["args"].isNull() && measureJson(reqDoc["args"]) >= plugin_jobs::kArgsLen)) {
        return sendJsonError(req, "400 Bad Request", "bad plugin/action/args");
    }
    plugin_jobs::Job* job = g_jobs.alloc(nowMs());
    if (job == nullptr) return sendJsonError(req, "503 Service Unavailable", "job queue full");
    snprintf(job->plugin, sizeof(job->plugin), "%s", plugin.c_str());
    snprintf(job->action, sizeof(job->action), "%s", action.c_str());
    if (!reqDoc["args"].isNull()) serializeJson(reqDoc["args"], job->args, sizeof(job->args));
    ESP_LOGI(TAG, "plugin job %u queued: %s/%s", static_cast<unsigned>(job->id), job->plugin, job->action);
    JsonDocument resp;
    resp["id"] = job->id;
    return sendJson(req, resp);
}

// GET /api/plugin-jobs/claim?plugin=<name> -> {id, claim, action, args} | {id:0}
esp_err_t pluginJobClaimHandler(httpd_req_t* req) {
    const std::string plugin = queryParam(req, "plugin");
    const plugin_jobs::Job* job = g_jobs.claim(plugin.c_str(), nowMs(), kJobLeaseMs);
    if (job == nullptr) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"id\":0}", HTTPD_RESP_USE_STRLEN);
    }
    std::string msg = "{\"id\":" + std::to_string(job->id) + ",\"claim\":" + std::to_string(job->claim) +
                      ",\"action\":\"" + job->action + "\",\"args\":" +
                      (job->args[0] ? job->args : "{}") + "}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, msg.c_str(), msg.size());
}

// POST /api/plugin-jobs/complete {id, claim, ok, result?} -> {ok}
// 409 when the claim is stale: the lease expired and another runner re-claimed
// the job, so this late result must not overwrite that runner's.
esp_err_t pluginJobCompleteHandler(httpd_req_t* req) {
    JsonDocument reqDoc;
    if (!readJsonBody(req, reqDoc)) return ESP_OK;
    const uint32_t id = reqDoc["id"] | 0;
    const uint32_t claim = reqDoc["claim"] | 0;
    const bool ok = reqDoc["ok"] | false;
    std::string resultJson;
    if (!reqDoc["result"].isNull()) {
        if (measureJson(reqDoc["result"]) >= plugin_jobs::kResultLen) {
            resultJson = "{\"error\":\"result too large\"}";
        } else {
            serializeJson(reqDoc["result"], resultJson);
        }
    }
    switch (g_jobs.complete(id, claim, ok, resultJson.empty() ? nullptr : resultJson.c_str(), nowMs())) {
        case plugin_jobs::Outcome::Ok:
            ESP_LOGI(TAG, "plugin job %u %s", static_cast<unsigned>(id), ok ? "done" : "failed");
            [[fallthrough]];
        case plugin_jobs::Outcome::AlreadySettled: {
            JsonDocument resp;  // answering twice is harmless
            resp["ok"] = true;
            return sendJson(req, resp);
        }
        case plugin_jobs::Outcome::StaleClaim:
            ESP_LOGI(TAG, "plugin job %u: stale completion ignored", static_cast<unsigned>(id));
            return sendJsonError(req, "409 Conflict", "stale claim");
        case plugin_jobs::Outcome::NotFound:
            break;
    }
    return sendJsonError(req, "404 Not Found", "no such running job");
}

// GET /api/plugin-jobs/status?id=<n> -> {id, state, result}
esp_err_t pluginJobStatusHandler(httpd_req_t* req) {
    const uint32_t id = strtoul(queryParam(req, "id").c_str(), nullptr, 10);
    static constexpr const char* kStateNames[] = {"empty", "pending", "running", "done", "error"};
    const plugin_jobs::Job* job = g_jobs.find(id);
    if (job != nullptr) {
        std::string msg = "{\"id\":" + std::to_string(id) + ",\"state\":\"" +
                          kStateNames[job->state & 0x07] + "\",\"result\":" +
                          (job->result[0] ? job->result : "null") + "}";
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, msg.c_str(), msg.size());
    }
    // Unknown: never existed, or its slot was recycled after completion.
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"id\":%u,\"state\":\"unknown\",\"result\":null}",
             static_cast<unsigned>(id));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// POST /api/plugin-fs?plugin=<name>&path=<path> with the file contents as a
// multipart file part. Multipart rather than a raw body: the parser streams
// file parts in chunks, so binary data (embedded NULs) and a body larger than
// the request header buffer survive. Writes <path>.tmp and renames on success,
// so an interrupted upload never replaces the previous file.
// ---------------------------------------------------------------------------

constexpr size_t kMaxPluginFile = 256 * 1024;

struct FsUpload {
    bool started = false;
    bool busy = false;
    std::string path;
    std::string tmp;
    HalFile file;
    size_t bytes = 0;
    int errorStatus = 0;
    const char* error = nullptr;
};

FsUpload g_fsUpload;

bool fsStart(void* ctx, const std::string& /*filename*/) {
    auto* up = static_cast<FsUpload*>(ctx);
    up->started = true;
    up->tmp = up->path + ".tmp";
    Storage.ensureDirectoryExists(parentOf(up->tmp).c_str());
    Storage.remove(up->tmp.c_str());
    if (!Storage.openFileForWrite("PLG", up->tmp, up->file)) {
        up->errorStatus = 500;
        up->error = "cannot write";
        return false;
    }
    return true;
}

bool fsWrite(void* ctx, const uint8_t* data, size_t len) {
    auto* up = static_cast<FsUpload*>(ctx);
    if (up->errorStatus != 0) return false;
    if (len > kMaxPluginFile - up->bytes) {
        up->errorStatus = 413;
        up->error = "too large";
        return false;
    }
    if (up->file.write(data, len) != len) {
        up->errorStatus = 500;
        up->error = "sd write failed";
        return false;
    }
    up->bytes += len;
    return true;
}

void fsFinish(void* ctx) {
    auto* up = static_cast<FsUpload*>(ctx);
    if (up->file.isOpen()) up->file.close();
    if (up->errorStatus != 0) return;
    if (up->bytes == 0) {
        up->errorStatus = 400;
        up->error = "empty body";
        return;
    }
    if (!Storage.replaceFile(up->tmp.c_str(), up->path.c_str())) {
        up->errorStatus = 500;
        up->error = "sd write failed";
    }
}

esp_err_t pluginFsHandler(httpd_req_t* req) {
    if (g_fsUpload.busy) return sendJsonError(req, "503 Service Unavailable", "upload in progress");
    // Explicit reset: FsUpload holds a HalFile, so it is reset field by field
    // rather than assigned from a temporary.
    if (g_fsUpload.file.isOpen()) g_fsUpload.file.close();
    g_fsUpload.started = false;
    g_fsUpload.path.clear();
    g_fsUpload.tmp.clear();
    g_fsUpload.bytes = 0;
    g_fsUpload.errorStatus = 0;
    g_fsUpload.error = nullptr;
    // The plugin argument is validated for shape even though the destination is
    // the explicit `path`: it is what the reference checks, and it keeps a
    // malformed caller from believing the write was attributed to a plugin.
    const std::string plugin = queryParam(req, "plugin");
    const std::string path = queryParam(req, "path");
    if (!safeComponent(plugin) || !isPluginPath(path)) {
        ESP_LOGW(TAG, "rejected plugin file write: plugin '%s' path '%s'", plugin.c_str(), path.c_str());
        return sendJsonError(req, "400 Bad Request", "bad path");
    }
    char type[160] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK) {
        return sendJsonError(req, "400 Bad Request", "multipart required");
    }
    std::string boundary;
    if (!multipart::extractBoundary(type, boundary)) {
        return sendJsonError(req, "400 Bad Request", "no boundary");
    }
    if (req->content_len <= 0 ||
        static_cast<size_t>(req->content_len) > kMaxPluginFile + 64 * 1024) {
        return sendJsonError(req, "400 Bad Request", "bad size");
    }

    g_fsUpload.busy = true;
    g_fsUpload.path = path;
    multipart::Sink sink{fsStart, fsWrite, fsFinish, &g_fsUpload, nullptr};
    multipart::Parser parser(boundary, sink);
    std::vector<char> buffer(kChunk);
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        const int want = std::min<int>(remaining, static_cast<int>(buffer.size()));
        const int got = httpd_req_recv(req, buffer.data(), want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
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
    if (g_fsUpload.file.isOpen()) g_fsUpload.file.close();
    // No file part at all (a form without a file field): nothing was stalled
    // and nothing may be reported as written.
    if (ok && g_fsUpload.errorStatus == 0 && !g_fsUpload.started) {
        g_fsUpload.errorStatus = 400;
        g_fsUpload.error = "missing file part";
    }
    if (!ok || g_fsUpload.errorStatus != 0) {
        if (!g_fsUpload.tmp.empty()) Storage.remove(g_fsUpload.tmp.c_str());
        const int status = g_fsUpload.errorStatus != 0 ? g_fsUpload.errorStatus : 400;
        const char* message = g_fsUpload.error != nullptr ? g_fsUpload.error : "upload failed";
        g_fsUpload.busy = false;
        switch (status) {
            case 413: return sendJsonError(req, "413 Payload Too Large", message);
            case 500: return sendJsonError(req, "500 Internal Server Error", message);
            default: return sendJsonError(req, "400 Bad Request", message);
        }
    }
    const size_t written = g_fsUpload.bytes;
    g_fsUpload.busy = false;
    JsonDocument resp;
    resp["ok"] = true;
    resp["bytes"] = written;
    return sendJson(req, resp);
}

// ---------------------------------------------------------------------------
// POST /api/fetch {plugin, url, dest, headers?, offset?, maxBytes?}
//   -> {status, bytes, complete, total?}
// The device downloads straight to SD so a large body never crosses the
// browser. Resumable and bounded: the page asks for 2 MB segments and resumes
// from `bytes` while `complete` is false.
// ---------------------------------------------------------------------------

constexpr size_t kFetchMaxSegment = 4 * 1024 * 1024;

esp_err_t pluginFetchHandler(httpd_req_t* req) {
    JsonDocument reqDoc;
    if (!readJsonBody(req, reqDoc, 16 * 1024)) return ESP_OK;
    const std::string url = reqDoc["url"] | "";
    const std::string dest = reqDoc["dest"] | "";
    const size_t requestedOffset =
        reqDoc["offset"].is<uint32_t>() ? static_cast<size_t>(reqDoc["offset"].as<uint32_t>()) : 0;
    size_t segmentLimit =
        reqDoc["maxBytes"].is<uint32_t>() ? static_cast<size_t>(reqDoc["maxBytes"].as<uint32_t>()) : 0;
    if (segmentLimit > kFetchMaxSegment) segmentLimit = kFetchMaxSegment;
    if (url.empty() || !isPluginPath(dest)) {
        return sendJsonError(req, "400 Bad Request", "bad url/dest");
    }
    std::vector<std::pair<std::string, std::string>> headers;
    readHeaders(reqDoc["headers"], headers);
    reqDoc.clear();

    if (WiFi.status() != WL_CONNECTED) {
        return sendJsonError(req, "502 Bad Gateway", "no wifi");
    }

    // Stage in <dest>.part so an interrupted download never sits under the real
    // name and an existing dest survives until the new copy is complete.
    const std::string part = dest + ".part";
    HalFile file;
    if (requestedOffset == 0) {
        Storage.ensureDirectoryExists(parentOf(part).c_str());
        Storage.remove(part.c_str());
        if (!Storage.openFileForWrite("PLG", part, file)) {
            return sendJsonError(req, "500 Internal Server Error", "cannot create file");
        }
    } else {
        file = Storage.open(part.c_str(), O_RDWR | O_AT_END);
        const size_t existing = file ? file.size() : 0;
        if (!file || existing != requestedOffset) {
            if (file) file.close();
            char message[96];
            snprintf(message, sizeof(message), "offset mismatch (%u)", static_cast<unsigned>(existing));
            return sendJsonError(req, "409 Conflict", message);
        }
    }

    size_t written = requestedOffset;
    bool sdFull = false;
    bool segmentBoundary = false;
    bool rangeUnsupported = false;

    // A phone may discard a response that sends no bytes for several minutes
    // while the device is still downloading upstream. Once the transfer turns
    // long-running, start a chunked JSON response and emit JSON whitespace
    // between chunks to keep the browser-facing connection alive. The final
    // result then goes out as the last chunk (this is what the reference does).
    bool browserStarted = false;
    uint32_t lastHeartbeat = nowMs();
    const auto keepBrowserAlive = [&]() {
        const uint32_t now = nowMs();
        if (now - lastHeartbeat < 5000) return;
        lastHeartbeat = now;
        if (!browserStarted) {
            httpd_resp_set_type(req, "application/json");
            if (httpd_resp_send_chunk(req, " ", 1) != ESP_OK) return;
            browserStarted = true;
        } else {
            httpd_resp_send_chunk(req, " \n", 2);
        }
    };
    const auto sendFetchResult = [&](JsonDocument& doc, const char* status) {
        String out;
        serializeJson(doc, out);
        if (!browserStarted) return sendJson(req, doc, status);
        httpd_resp_send_chunk(req, out.c_str(), out.length());
        return httpd_resp_send_chunk(req, nullptr, 0);
    };

    freeink::FetchOptions options;
    options.startOffset = requestedOffset;
    freeink::FetchSink sink;
    sink.write = [&](const uint8_t* data, size_t len) {
        const size_t writeLen =
            segmentLimit > 0 ? std::min(len, requestedOffset + segmentLimit - written) : len;
        if (file.write(data, writeLen) != writeLen) {
            sdFull = true;
            return false;
        }
        written += writeLen;
        keepBrowserAlive();
        if (segmentLimit > 0 && written - requestedOffset >= segmentLimit) {
            segmentBoundary = true;
            return false;
        }
        return true;
    };
    sink.rewind = [&] {
        // Range ignored: the body restarts from byte 0, which only a transfer
        // that has not yet reported progress to the browser can follow.
        if (requestedOffset > 0) {
            rangeUnsupported = true;
            return false;
        }
        file.close();
        written = 0;
        return Storage.openFileForWrite("PLG", part, file);
    };
    const freeink::FetchResult result = freeink::fetchResumable(
        url, options,
        [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
            http.setUserAgent("CPYOS");
            // No CA bundle ships with this transport (the esp_tls client is
            // created insecure elsewhere too): traffic stays encrypted, peer
            // identity is not verified.
            http.setInsecure();
            // Delivery servers that assemble books on the fly stall mid-body.
            http.setTimeout(60000);
            http.setReuse(false);
            if (sameOrigin) {
                for (const auto& header : headers) http.addHeader(header.first, header.second);
            }
        },
        sink);

    if (file.isOpen()) {
        file.flush();
        file.close();
    }
    const int status = result.status;
    const size_t totalExpected = result.total;
    bool complete = result.complete ||
                    (segmentBoundary && totalExpected > 0 && written >= totalExpected);
    const bool ok2xx = status >= 200 && status < 300;
    const bool midSegment = segmentBoundary && !complete && ok2xx;

    JsonDocument resp;
    if (!complete && ok2xx && !midSegment) {
        Storage.remove(part.c_str());
        resp["error"] = sdFull ? "sd write failed" : rangeUnsupported ? "range unsupported"
                                                                     : "download truncated";
        resp["bytes"] = written;
        resp["complete"] = false;
        // The URL is never logged: query strings can carry tokens.
        ESP_LOGE(TAG, "plugin fetch failed after %u bytes", static_cast<unsigned>(written));
        return sendFetchResult(resp, "502 Bad Gateway");
    }
    if (!ok2xx) {
        Storage.remove(part.c_str());
        resp["error"] = status < 0 ? "transport failure" : "http status";
    } else if (complete && !Storage.replaceFile(part.c_str(), dest.c_str())) {
        Storage.remove(part.c_str());
        complete = false;
        resp["error"] = "sd write failed";
    }
    resp["status"] = status;
    resp["bytes"] = written;
    resp["complete"] = complete;
    if (totalExpected > 0) resp["total"] = totalExpected;
    return sendFetchResult(resp, nullptr);
}

// ---------------------------------------------------------------------------
// POST /api/relay {plugin, method, url, headers:{}, body}
//   -> 200 with the upstream body raw, its status in X-Relay-Status and its
//      headers in X-Relay-Headers ([[name, value], ...] JSON, duplicates kept).
// A plugin needs this for CORS-forbidden endpoints; sending the body raw avoids
// escaping it into JSON on the device.
// ---------------------------------------------------------------------------

constexpr size_t kRelayBodyLimit = 32 * 1024;

esp_err_t pluginRelayHandler(httpd_req_t* req) {
    JsonDocument reqDoc;
    if (!readJsonBody(req, reqDoc, 16 * 1024)) return ESP_OK;
    const String plugin = reqDoc["plugin"] | "";
    const std::string url = reqDoc["url"] | "";
    const std::string method = reqDoc["method"] | "GET";
    if (!safeComponent(plugin.c_str()) || url.empty()) {
        return sendJsonError(req, "400 Bad Request", "missing plugin/url");
    }
    std::vector<std::pair<std::string, std::string>> headers;
    readHeaders(reqDoc["headers"], headers);
    const std::string body = reqDoc["body"] | "";
    reqDoc.clear();

    if (WiFi.status() != WL_CONNECTED) {
        return sendJsonError(req, "502 Bad Gateway", "no wifi");
    }

    freeink::SecureHttpClient http;
    http.setUserAgent("CPYOS");
    http.setInsecure();
    http.setTimeout(30000);
    http.setReuse(false);
    if (!http.begin(url)) return sendJsonError(req, "400 Bad Request", "bad url");
    for (const auto& header : headers) http.addHeader(header.first, header.second);

    std::string response;
    response.reserve(4096);
    bool tooLarge = false;
    const int status = http.sendRequest(method.c_str(), reinterpret_cast<const uint8_t*>(body.data()),
                                        body.size(), [&](const uint8_t* data, size_t len) {
                                            if (response.size() + len > kRelayBodyLimit) {
                                                tooLarge = true;
                                                return false;
                                            }
                                            response.append(reinterpret_cast<const char*>(data), len);
                                            return true;
                                        });
    if (status < 0 || tooLarge) {
        ESP_LOGW(TAG, "relay failed (status %d, %u bytes)", status, static_cast<unsigned>(response.size()));
        return sendJsonError(req, "502 Bad Gateway",
                             tooLarge ? "response too large; large bodies need /api/fetch"
                                      : "relay failed");
    }

    std::string headersJson = "[";
    bool first = true;
    size_t count = 0;
    for (const auto& header : http.getHeaders()) {
        if (count++ >= 32) break;
        if (!first) headersJson += ',';
        first = false;
        headersJson += '[';
        std::string escaped;
        jsonEscape(header.first, escaped);
        headersJson += '"' + escaped + "\",\"";
        escaped.clear();
        jsonEscape(header.second, escaped);
        headersJson += escaped + "\"]";
    }
    headersJson += ']';

    char statusText[16];
    snprintf(statusText, sizeof(statusText), "%d", status);
    httpd_resp_set_hdr(req, "X-Relay-Status", statusText);
    // The header value is read from this string during the send below, so it
    // must outlive the response.
    httpd_resp_set_hdr(req, "X-Relay-Headers", headersJson.c_str());
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, response.data(), response.size());
}

// ---------------------------------------------------------------------------
// POST /api/crypto {op, ...base64 fields...} -> {data|...}
// The reference implements this vocabulary on wolfSSL. This port links no
// wolfSSL, so the primitives a plugin actually needs are served from PSA
// Crypto (the same backend the port already uses for protected EPUBs):
//   random, sha1, sha256, aesenc, aesdec
// The RSA/PKCS#12 operations (rsadec, keygen, pubencrypt, sign, pkcs12) belong
// to the legacy off-device rights scheme; PSA on this board has no imported
// RSA/ECB keystore path, so they are refused explicitly instead of faked.
// ---------------------------------------------------------------------------

bool psaReady() {
    static const bool ready = psa_crypto_init() == PSA_SUCCESS;
    return ready;
}

bool aesCbc(const bool encrypt, const uint8_t key[16], const uint8_t iv[16], const uint8_t* in,
            size_t len, uint8_t* out, size_t outCap, size_t* outLen) {
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CBC_NO_PADDING);
    mbedtls_svc_key_id_t keyId = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_import_key(&attributes, key, 16, &keyId) != PSA_SUCCESS) return false;

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status = encrypt ? psa_cipher_encrypt_setup(&op, keyId, PSA_ALG_CBC_NO_PADDING)
                                  : psa_cipher_decrypt_setup(&op, keyId, PSA_ALG_CBC_NO_PADDING);
    size_t updateLen = 0;
    size_t finishLen = 0;
    if (status == PSA_SUCCESS) status = psa_cipher_set_iv(&op, iv, 16);
    if (status == PSA_SUCCESS) status = psa_cipher_update(&op, in, len, out, outCap, &updateLen);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&op, out + updateLen, outCap - updateLen, &finishLen);
    }
    psa_cipher_abort(&op);
    psa_destroy_key(keyId);
    if (status != PSA_SUCCESS) return false;
    *outLen = updateLen + finishLen;
    return true;
}

struct CryptoBytes {
    std::string data;
    const uint8_t* ptr() const {
        static constexpr uint8_t kEmpty = 0;
        return data.empty() ? &kEmpty : reinterpret_cast<const uint8_t*>(data.data());
    }
    size_t size() const { return data.size(); }
};

std::string cryptoDecodeField(JsonDocument& req, const char* field, bool& bad) {
    const char* value = req[field].as<const char*>();
    if (value == nullptr) return {};
    const size_t encodedLen = strlen(value);
    if (encodedLen == 0 || encodedLen > 64 * 1024) {
        bad = true;
        return {};
    }
    std::string out;
    out.resize(encodedLen * 3 / 4 + 3);
    const int32_t n = freeink::content::base64Decode(value, encodedLen,
                                                    reinterpret_cast<uint8_t*>(out.data()), out.size());
    if (n < 0) {
        bad = true;
        return {};
    }
    out.resize(static_cast<size_t>(n));
    return out;
}

esp_err_t pluginCryptoHandler(httpd_req_t* req) {
    JsonDocument reqDoc;
    if (!readJsonBody(req, reqDoc, 96 * 1024)) return ESP_OK;
    const std::string op = reqDoc["op"] | "";

    JsonDocument resp;
    const auto setEncoded = [&resp](const char* name, const uint8_t* data, size_t size) {
        static const uint8_t kEmpty = 0;
        if (size == 0) data = &kEmpty;
        resp[name] = base64::encode(data, size);
    };

    bool bad = false;
    if (op == "random") {
        static constexpr int kMaxRandomBytes = 4096;
        int n = reqDoc["len"] | 16;
        n = std::max(0, std::min(n, kMaxRandomBytes));
        std::vector<uint8_t> out(static_cast<size_t>(n));
        if (n > 0) esp_fill_random(out.data(), out.size());
        setEncoded("data", out.data(), out.size());
    } else if (op == "sha1" || op == "sha256") {
        const CryptoBytes d{cryptoDecodeField(reqDoc, "data", bad)};
        if (bad) return sendJsonError(req, "400 Bad Request", "bad base64");
        uint8_t digest[32] = {};
        const size_t len = op == "sha1" ? 20 : 32;
        size_t written = 0;
        const psa_algorithm_t algorithm = op == "sha1" ? PSA_ALG_SHA_1 : PSA_ALG_SHA_256;
        if (!psaReady() ||
            psa_hash_compute(algorithm, d.ptr(), d.size(), digest, len, &written) != PSA_SUCCESS ||
            written != len) {
            resp["error"] = "hash failed";
        } else {
            setEncoded("data", digest, len);
        }
    } else if (op == "aesenc" || op == "aesdec") {
        const CryptoBytes k{cryptoDecodeField(reqDoc, "key", bad)};
        const CryptoBytes iv{cryptoDecodeField(reqDoc, "iv", bad)};
        const CryptoBytes d{cryptoDecodeField(reqDoc, "data", bad)};
        if (bad) return sendJsonError(req, "400 Bad Request", "bad base64");
        if (k.size() != 16 || iv.size() != 16) {
            resp["error"] = "key/iv must be 16 bytes";
        } else if (op == "aesenc") {
            // PKCS#7 padding, as the reference does for aesenc.
            const size_t outSize = (d.size() / 16 + 1) * 16;
            std::vector<uint8_t> out(outSize);
            memcpy(out.data(), d.ptr(), d.size());
            memset(out.data() + d.size(), static_cast<int>(outSize - d.size()), outSize - d.size());
            size_t outLen = 0;
            if (psaReady() && aesCbc(true, k.ptr(), iv.ptr(), out.data(), outSize, out.data(), out.size(),
                                     &outLen) &&
                outLen == outSize) {
                setEncoded("data", out.data(), outLen);
            } else {
                resp["error"] = "aesenc failed";
            }
        } else if (d.size() % 16 != 0) {
            resp["error"] = "data not block-aligned";
        } else {
            std::vector<uint8_t> out(d.size() == 0 ? 1 : d.size());
            size_t outLen = 0;
            if (psaReady() && aesCbc(false, k.ptr(), iv.ptr(), d.ptr(), d.size(), out.data(), out.size(),
                                     &outLen) &&
                outLen == d.size()) {
                setEncoded("data", out.data(), outLen);
            } else {
                resp["error"] = "aesdec failed";
            }
        }
    } else if (op == "rsadec" || op == "keygen" || op == "pubencrypt" || op == "sign" ||
               op == "pkcs12") {
        // TODO(plus): the reference serves these from wolfSSL's RSA/PKCS#12
        // helpers. This port has no wolfSSL, and the PSA backend it uses has no
        // import path for a caller-supplied PKCS#8 key or PKCS#12 bundle, so
        // the operations are refused rather than emulated.
        resp["error"] = "op not available on this port (no wolfSSL)";
    } else {
        resp["error"] = "unknown op";
    }

    if (resp.overflowed()) return sendJsonError(req, "503 Service Unavailable", "out of memory");
    return sendJson(req, resp);
}

// ---------------------------------------------------------------------------
// POST /api/book-key {path, key (base64, 16 bytes), expires?} -> {ok}
// Stores a protected book's content key, wrapped to this device, as
// "<path>.key" for the reader to open the book with.
// ---------------------------------------------------------------------------

esp_err_t bookKeyHandler(httpd_req_t* req) {
    JsonDocument reqDoc;
    if (!readJsonBody(req, reqDoc)) return ESP_OK;
    const std::string path = reqDoc["path"] | "";
    const int64_t expires = reqDoc["expires"] | static_cast<int64_t>(0);
    bool bad = false;
    const CryptoBytes decoded{cryptoDecodeField(reqDoc, "key", bad)};
    if (bad || decoded.size() != bookkey::KEY_LEN) {
        return sendJsonError(req, "400 Bad Request", "bad key");
    }
    if (!isPluginPath(path) || expires < 0) {
        return sendJsonError(req, "400 Bad Request", "bad path");
    }
    uint8_t key[bookkey::KEY_LEN];
    memcpy(key, decoded.ptr(), sizeof(key));
    if (!bookkey::write(path, key, expires)) {
        return sendJsonError(req, "500 Internal Server Error", "cannot store key");
    }
    JsonDocument resp;
    resp["ok"] = true;
    return sendJson(req, resp);
}

httpd_uri_t makeUri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t entry{};
    entry.uri = uri;
    entry.method = method;
    entry.handler = handler;
    entry.user_ctx = nullptr;
    return entry;
}

}  // namespace

bool registerRoutes(httpd_handle_t server) {
    if (server == nullptr) return false;
    const httpd_uri_t routes[] = {
        makeUri("/api/plugins", HTTP_GET, pluginListHandler),
        makeUri("/plugin", HTTP_GET, pluginFileHandler),
        makeUri("/plugins-run", HTTP_GET, pluginRunnerPageHandler),
        makeUri("/api/plugin-jobs", HTTP_POST, pluginJobSubmitHandler),
        makeUri("/api/plugin-jobs/claim", HTTP_GET, pluginJobClaimHandler),
        makeUri("/api/plugin-jobs/complete", HTTP_POST, pluginJobCompleteHandler),
        makeUri("/api/plugin-jobs/status", HTTP_GET, pluginJobStatusHandler),
        makeUri("/api/plugin-fs", HTTP_POST, pluginFsHandler),
        makeUri("/api/fetch", HTTP_POST, pluginFetchHandler),
        makeUri("/api/relay", HTTP_POST, pluginRelayHandler),
        makeUri("/api/crypto", HTTP_POST, pluginCryptoHandler),
        makeUri("/api/book-key", HTTP_POST, bookKeyHandler),
    };
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            ESP_LOGE(TAG, "cannot register %s", route.uri);
            return false;
        }
    }
    ESP_LOGI(TAG, "plugin routes ready (%u)", static_cast<unsigned>(sizeof(routes) / sizeof(routes[0])));
    return true;
}

}  // namespace web_plugins
