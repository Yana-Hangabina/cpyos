// OPDS / Wi-Fi credential APIs for the Plus web server.
//
// Same JSON contract and status codes as the shared CrossPointWebServer:
// listing never exposes passwords, a POST without `password` keeps the stored
// one (the web form omits it when unchanged), and deletion is a POST with an
// index because the reference's WebServer had no DELETE-with-body.

#include "web_stores.h"

#include <ArduinoJson.h>
#include <OpdsServerStore.h>
#include <WifiCredentialStore.h>
#include <esp_log.h>

#include <cstdint>
#include <string>

namespace web_stores {
namespace {

constexpr const char* TAG = "PLUS-STORE";

esp_err_t sendText(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t sendJson(httpd_req_t* req, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out.c_str(), out.length());
}

bool readJsonBody(httpd_req_t* req, JsonDocument& doc) {
    if (req->content_len <= 0 || req->content_len > 16384) {
        sendText(req, "400 Bad Request", "bad body size");
        return false;
    }
    std::string body(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int got = httpd_req_recv(req, &body[received], body.size() - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            sendText(req, "400 Bad Request", "short body");
            return false;
        }
        received += static_cast<size_t>(got);
    }
    if (deserializeJson(doc, body.data(), body.size()) != DeserializationError::Ok) {
        sendText(req, "400 Bad Request", "invalid JSON");
        return false;
    }
    return true;
}

std::string field(JsonVariantConst doc, const char* key) {
    return doc[key] | std::string("");
}

// --------------------------------------------------------------------------
// OPDS servers
// --------------------------------------------------------------------------

esp_err_t opdsList(httpd_req_t* req) {
    const auto& servers = OPDS_STORE.getServers();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < servers.size(); ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["index"] = i;
        obj["name"] = servers[i].name;
        obj["url"] = servers[i].url;
        obj["username"] = servers[i].username;
        obj["hasPassword"] = !servers[i].password.empty();
    }
    return sendJson(req, doc);
}

esp_err_t opdsUpsert(httpd_req_t* req) {
    JsonDocument doc;
    if (!readJsonBody(req, doc)) return ESP_OK;
    OpdsServer server;
    server.name = field(doc, "name");
    server.url = field(doc, "url");
    server.username = field(doc, "username");
    if (server.name.size() > 128 || server.url.size() > 512 || server.username.size() > 128) {
        return sendText(req, "400 Bad Request", "field too long");
    }
    const bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
    server.password = field(doc, "password");
    if (server.password.size() > 256) return sendText(req, "400 Bad Request", "field too long");

    if (doc["index"].is<int>()) {
        const int idx = doc["index"].as<int>();
        if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
            return sendText(req, "400 Bad Request", "Invalid server index");
        }
        if (!hasPasswordField) {
            const OpdsServer* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
            if (existing != nullptr) server.password = existing->password;
        }
        if (!OPDS_STORE.updateServer(static_cast<size_t>(idx), server)) {
            return sendText(req, "500 Internal Server Error", "cannot save");
        }
        ESP_LOGI(TAG, "OPDS server %d updated", idx);
    } else {
        if (!OPDS_STORE.addServer(server)) {
            return sendText(req, "400 Bad Request", "Cannot add server (limit reached)");
        }
        ESP_LOGI(TAG, "OPDS server added (count=%u)", static_cast<unsigned>(OPDS_STORE.getCount()));
    }
    return sendText(req, "200 OK", "OK");
}

esp_err_t opdsDelete(httpd_req_t* req) {
    JsonDocument doc;
    if (!readJsonBody(req, doc)) return ESP_OK;
    if (!doc["index"].is<int>()) return sendText(req, "400 Bad Request", "Missing index");
    const int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
        return sendText(req, "400 Bad Request", "Invalid server index");
    }
    if (!OPDS_STORE.removeServer(static_cast<size_t>(idx))) {
        return sendText(req, "500 Internal Server Error", "cannot save");
    }
    ESP_LOGI(TAG, "OPDS server %d deleted", idx);
    return sendText(req, "200 OK", "OK");
}

// --------------------------------------------------------------------------
// Wi-Fi credentials
// --------------------------------------------------------------------------

esp_err_t wifiList(httpd_req_t* req) {
    const auto credentials = WIFI_STORE.getCredentialSummaries();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < credentials.size(); ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["index"] = i;
        obj["ssid"] = credentials[i].ssid;
        obj["hasPassword"] = credentials[i].hasPassword;
        obj["isLastConnected"] = credentials[i].isLastConnected;
    }
    return sendJson(req, doc);
}

esp_err_t wifiUpsert(httpd_req_t* req) {
    JsonDocument doc;
    if (!readJsonBody(req, doc)) return ESP_OK;
    const std::string ssid = field(doc, "ssid");
    if (ssid.empty()) return sendText(req, "400 Bad Request", "SSID is required");
    if (ssid.size() > 32) return sendText(req, "400 Bad Request", "SSID too long");
    const bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
    std::string password = field(doc, "password");
    if (password.size() > 64) return sendText(req, "400 Bad Request", "password too long");

    bool ok = true;
    if (doc["index"].is<int>()) {
        const int idx = doc["index"].as<int>();
        if (idx < 0) return sendText(req, "400 Bad Request", "Invalid network index");
        const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
        if (!credential) return sendText(req, "400 Bad Request", "Invalid network index");
        if (!hasPasswordField) password = credential->password;
        if (credential->ssid != ssid) {
            ok = WIFI_STORE.removeCredential(credential->ssid) && WIFI_STORE.addCredential(ssid, password);
        } else {
            ok = WIFI_STORE.addCredential(ssid, password);
        }
        if (!ok) return sendText(req, "400 Bad Request", "Failed to update Wi-Fi network");
        ESP_LOGI(TAG, "Wi-Fi network %d updated", idx);
    } else {
        if (!WIFI_STORE.addCredential(ssid, password)) {
            return sendText(req, "400 Bad Request", "Cannot add network (limit reached)");
        }
        ESP_LOGI(TAG, "Wi-Fi network added (count=%u)",
                 static_cast<unsigned>(WIFI_STORE.getCredentialCount()));
    }
    return sendText(req, "200 OK", "OK");
}

esp_err_t wifiDelete(httpd_req_t* req) {
    JsonDocument doc;
    if (!readJsonBody(req, doc)) return ESP_OK;
    if (!doc["index"].is<int>()) return sendText(req, "400 Bad Request", "Missing index");
    const int idx = doc["index"].as<int>();
    if (idx < 0) return sendText(req, "400 Bad Request", "Invalid network index");
    const auto ssid = WIFI_STORE.getSsidAt(static_cast<size_t>(idx));
    if (!ssid) return sendText(req, "400 Bad Request", "Invalid network index");
    if (!WIFI_STORE.removeCredential(*ssid)) {
        return sendText(req, "400 Bad Request", "Failed to delete Wi-Fi network");
    }
    ESP_LOGI(TAG, "Wi-Fi network %d deleted", idx);
    return sendText(req, "200 OK", "OK");
}

httpd_uri_t makeUri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t entry{};
    entry.uri = uri;
    entry.method = method;
    entry.handler = handler;
    return entry;
}

}  // namespace

bool registerRoutes(httpd_handle_t server) {
    if (server == nullptr) return false;
    const httpd_uri_t routes[] = {
        makeUri("/api/opds", HTTP_GET, opdsList),
        makeUri("/api/opds", HTTP_POST, opdsUpsert),
        makeUri("/api/opds/delete", HTTP_POST, opdsDelete),
        makeUri("/api/wifi", HTTP_GET, wifiList),
        makeUri("/api/wifi", HTTP_POST, wifiUpsert),
        makeUri("/api/wifi/delete", HTTP_POST, wifiDelete),
    };
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            ESP_LOGE(TAG, "cannot register %s", route.uri);
            return false;
        }
    }
    return true;
}

}  // namespace web_stores
