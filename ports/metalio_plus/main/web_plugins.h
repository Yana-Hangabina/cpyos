#pragma once

// Browser-side JS plugins for the Metalio Plus web server.
//
// The reference firmware keeps plugin logic off the device: the JS lives on
// the SD card and runs in the browser; the firmware only stores small JSON
// jobs and exposes a handful of generic capabilities (TLS relay, disk-backed
// fetch, crypto primitives, a book content key). This file ports that surface
// onto the IDF httpd used by main/web_transfer.cpp.
//
// The routes are registered by web_transfer::start() *before* WebDAV, whose
// "/*" handler is the wildcard fallback for arbitrary file paths: the esp_http
// server picks the first handler whose method matches, so an exact URI has to
// come first.

#include <esp_http_server.h>

namespace web_plugins {

// Registers /api/plugins, /plugin, /plugins-run, the plugin job queue,
// /api/plugin-fs, /api/fetch, /api/relay, /api/crypto and /api/book-key.
// Returns false when any handler could not be registered (the caller logs it;
// the rest of the file-transfer server stays usable).
bool registerRoutes(httpd_handle_t server);

}  // namespace web_plugins
