// Minimal read/write WebDAV for the SD card (file-manager clients).
//
// Supported: OPTIONS, PROPFIND (Depth 0/1), GET/HEAD, PUT, DELETE, MKCOL,
// MOVE, COPY (files), LOCK/UNLOCK (dummy token) and PROPPATCH (no-op). Paths go
// through the same guards as the HTTP API: under /sdcard, no "..", no
// /.crosspoint. Handlers are registered on "/*" after the exact endpoints, so
// /, /api/*, /download and /upload keep their own handlers.
//
// UNTESTED ON HARDWARE.

#include "webdav.h"

#include "form_fields.h"
#include "transfer_paths.h"
#include "webdav_format.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace webdav {
namespace {

constexpr const char* TAG = "PLUS-DAV";
constexpr size_t kChunk = 4096;

// Clients send XML bodies with PROPFIND/PROPPATCH/LOCK/MOVE/COPY. The handlers
// ignore the body, but it must still be consumed before the response or a
// keep-alive connection would parse it as the next request.
void drainBody(httpd_req_t* req) {
    char buffer[256];
    int remaining = static_cast<int>(req->content_len);
    while (remaining > 0) {
        const int want = remaining < static_cast<int>(sizeof(buffer)) ? remaining : static_cast<int>(sizeof(buffer));
        const int got = httpd_req_recv(req, buffer, want);
        if (got <= 0) return;
        remaining -= got;
    }
}

bool logicalFor(httpd_req_t* req, std::string& logical) {
    std::string path = req->uri;
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    path = form_fields::urlDecode(path);
    if (path.empty() || path.front() != '/') return false;
    if (!web_transfer::paths::safe(path)) return false;
    logical = path;
    return true;
}

bool headerValue(httpd_req_t* req, const char* name, std::string& value) {
    char buffer[512] = {};
    if (httpd_req_get_hdr_value_str(req, name, buffer, sizeof(buffer)) != ESP_OK) return false;
    value = buffer;
    return true;
}

std::string propResponse(const std::string& logical, const struct stat& st) {
    const bool dir = S_ISDIR(st.st_mode);
    std::string href = hrefEncode(logical);
    if (dir && (href.empty() || href.back() != '/')) href += '/';
    const std::string display = xmlEscape(logical == "/" ? "/" : logical.substr(logical.find_last_of('/') + 1));
    std::string out = "<D:response><D:href>" + href + "</D:href><D:propstat><D:prop>";
    out += "<D:displayname>" + display + "</D:displayname>";
    out += "<D:getcontentlength>" + std::to_string(dir ? 0 : static_cast<long long>(st.st_size)) +
           "</D:getcontentlength>";
    out += "<D:getlastmodified>" + httpDate(st.st_mtime) + "</D:getlastmodified>";
    out += dir ? "<D:resourcetype><D:collection/></D:resourcetype>"
               : "<D:resourcetype/><D:getcontenttype>" +
                     std::string(contentTypeFor(logical)) + "</D:getcontenttype>";
    out += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
    return out;
}

esp_err_t sendStatusText(httpd_req_t* req, const char* status, const char* message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

esp_err_t sendXml(httpd_req_t* req, int status, const std::string& body) {
    httpd_resp_set_status(req, status == 207 ? "207 Multi-Status" : status == 201 ? "201 Created"
                            : status == 204 ? "204 No Content" : "200 OK");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t optionsHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "DAV", "1,2");
    httpd_resp_set_hdr(req, "Allow",
                       "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t propfindHandler(httpd_req_t* req) {
    drainBody(req);
    std::string logical;
    if (!logicalFor(req, logical)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    struct stat st = {};
    if (stat(web_transfer::paths::toReal(logical).c_str(), &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    std::string depthHeader;
    const int depth = headerValue(req, "Depth", depthHeader) ? parseDepth(depthHeader.c_str()) : 1;
    std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
    body += propResponse(logical, st);
    if (depth >= 1 && S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(web_transfer::paths::toReal(logical).c_str());
        if (dir != nullptr) {
            size_t count = 0;
            while (const dirent* entry = readdir(dir)) {
                const char* name = entry->d_name;
                if (name[0] == '.' || strcmp(name, "System Volume Information") == 0 ||
                    strcmp(name, "XTCache") == 0) {
                    continue;
                }
                if (++count > 500) break;
                std::string child = logical;
                if (child.empty() || child.back() != '/') child += '/';
                child += name;
                if (!web_transfer::paths::safe(child)) continue;
                struct stat childStat = {};
                if (stat(web_transfer::paths::toReal(child).c_str(), &childStat) == 0) {
                    body += propResponse(child, childStat);
                }
            }
            closedir(dir);
        }
    }
    body += "</D:multistatus>";
    return sendXml(req, 207, body);
}

esp_err_t getHandler(httpd_req_t* req) {
    std::string logical;
    if (!logicalFor(req, logical)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    struct stat st = {};
    const std::string real = web_transfer::paths::toReal(logical);
    if (stat(real.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const int fd = open(real.c_str(), O_RDONLY);
    if (fd < 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    httpd_resp_set_type(req, contentTypeFor(logical));
    if (req->method == HTTP_HEAD) {
        httpd_resp_set_hdr(req, "Content-Length", std::to_string(st.st_size).c_str());
        close(fd);
        return httpd_resp_send(req, nullptr, 0);
    }
    char buffer[kChunk];
    ssize_t got = 0;
    while ((got = read(fd, buffer, sizeof(buffer))) > 0) {
        if (httpd_resp_send_chunk(req, buffer, got) != ESP_OK) {
            close(fd);
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    close(fd);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t putHandler(httpd_req_t* req) {
    std::string logical;
    if (!logicalFor(req, logical) || logical == "/") {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string real = web_transfer::paths::toReal(logical);
    struct stat st = {};
    if (stat(real.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "is a directory");
    }
    const bool existed = stat(real.c_str(), &st) == 0;
    const std::string parent = real.substr(0, real.find_last_of('/'));
    struct stat parentStat = {};
    if (stat(parent.c_str(), &parentStat) != 0 || !S_ISDIR(parentStat.st_mode)) {
        return sendStatusText(req, "409 Conflict", "parent missing");
    }
    const std::string temp = real + ".davtmp";
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create");
    char buffer[kChunk];
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        const int want = remaining < static_cast<int>(sizeof(buffer)) ? remaining : static_cast<int>(sizeof(buffer));
        const int got = httpd_req_recv(req, buffer, want);
        if (got <= 0) { ok = false; break; }
        if (write(fd, buffer, got) != got) { ok = false; break; }
        remaining -= got;
    }
    close(fd);
    if (!ok) {
        unlink(temp.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    unlink(real.c_str());
    if (rename(temp.c_str(), real.c_str()) != 0) {
        unlink(temp.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    return sendXml(req, existed ? 204 : 201, "");
}

esp_err_t deleteHandler(httpd_req_t* req) {
    std::string logical;
    if (!logicalFor(req, logical) || logical == "/") {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string real = web_transfer::paths::toReal(logical);
    struct stat st = {};
    if (stat(real.c_str(), &st) != 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    const int rc = S_ISDIR(st.st_mode) ? rmdir(real.c_str()) : unlink(real.c_str());
    if (rc != 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "delete failed");
    return sendXml(req, 204, "");
}

esp_err_t mkcolHandler(httpd_req_t* req) {
    std::string logical;
    if (!logicalFor(req, logical) || logical == "/") {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const std::string real = web_transfer::paths::toReal(logical);
    struct stat st = {};
    if (stat(real.c_str(), &st) == 0) return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "exists");
    const std::string parent = real.substr(0, real.find_last_of('/'));
    struct stat parentStat = {};
    if (stat(parent.c_str(), &parentStat) != 0 || !S_ISDIR(parentStat.st_mode)) {
        return sendStatusText(req, "409 Conflict", "parent missing");
    }
    if (mkdir(real.c_str(), 0755) != 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
    }
    return sendXml(req, 201, "");
}

esp_err_t copyMoveHandler(httpd_req_t* req, bool move) {
    drainBody(req);
    std::string logical;
    if (!logicalFor(req, logical) || logical == "/") {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    std::string destination;
    if (!headerValue(req, "Destination", destination)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "destination required");
    }
    std::string target;
    if (!pathFromDestination(destination, target) || !web_transfer::paths::safe(target)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad destination");
    }
    std::string overwrite;
    const bool allowOverwrite = !headerValue(req, "Overwrite", overwrite) || overwriteAllowed(overwrite.c_str());
    const std::string source = web_transfer::paths::toReal(logical);
    const std::string dest = web_transfer::paths::toReal(target);
    struct stat st = {};
    if (stat(source.c_str(), &st) != 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    if (S_ISDIR(st.st_mode)) return httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "files only");
    struct stat existing = {};
    const bool destExists = stat(dest.c_str(), &existing) == 0;
    if (destExists && !allowOverwrite) {
        return sendStatusText(req, "412 Precondition Failed", "overwrite forbidden");
    }
    if (move) {
        unlink(dest.c_str());
        if (rename(source.c_str(), dest.c_str()) != 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "move failed");
        }
        return sendXml(req, destExists ? 204 : 201, "");
    }
    const int in = open(source.c_str(), O_RDONLY);
    if (in < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    const std::string temp = dest + ".davtmp";
    const int out = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create");
    }
    char buffer[kChunk];
    ssize_t got = 0;
    bool ok = true;
    while ((got = read(in, buffer, sizeof(buffer))) > 0) {
        if (write(out, buffer, got) != got) { ok = false; break; }
    }
    close(in);
    close(out);
    if (!ok) {
        unlink(temp.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "copy failed");
    }
    unlink(dest.c_str());
    if (rename(temp.c_str(), dest.c_str()) != 0) {
        unlink(temp.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }
    return sendXml(req, destExists ? 204 : 201, "");
}

esp_err_t lockHandler(httpd_req_t* req) {
    drainBody(req);
    std::string logical;
    if (!logicalFor(req, logical)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery>"
        "<D:activelock><D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope>"
        "<D:depth>infinity</D:depth><D:timeout>Second-3600</D:timeout>"
        "<D:locktoken><D:href>opaquelocktoken:cpyos-plus</D:href></D:locktoken>"
        "</D:activelock></D:lockdiscovery></D:prop>";
    httpd_resp_set_hdr(req, "Lock-Token", "<opaquelocktoken:cpyos-plus>");
    return sendXml(req, 200, body);
}

esp_err_t unlockHandler(httpd_req_t* req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t proppatchHandler(httpd_req_t* req) {
    drainBody(req);
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">"
        "<D:response><D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
        "</D:response></D:multistatus>";
    return sendXml(req, 207, body);
}

}  // namespace

httpd_uri_t makeUri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t entry{};
    entry.uri = uri;
    entry.method = method;
    entry.handler = handler;
    return entry;
}

bool registerHandlers(httpd_handle_t server) {
    const httpd_uri_t uris[] = {
        makeUri("/*", HTTP_OPTIONS, optionsHandler),
        makeUri("/*", HTTP_PROPFIND, propfindHandler),
        makeUri("/*", HTTP_GET, getHandler),
        makeUri("/*", HTTP_HEAD, getHandler),
        makeUri("/*", HTTP_PUT, putHandler),
        makeUri("/*", HTTP_DELETE, deleteHandler),
        makeUri("/*", HTTP_MKCOL, mkcolHandler),
        makeUri("/*", HTTP_MOVE, [](httpd_req_t* req) { return copyMoveHandler(req, true); }),
        makeUri("/*", HTTP_COPY, [](httpd_req_t* req) { return copyMoveHandler(req, false); }),
        makeUri("/*", HTTP_LOCK, lockHandler),
        makeUri("/*", HTTP_UNLOCK, unlockHandler),
        makeUri("/*", HTTP_PROPPATCH, proppatchHandler),
    };
    for (const auto& uri : uris) {
        if (httpd_register_uri_handler(server, &uri) != ESP_OK) return false;
    }
    return true;
}

}  // namespace webdav
