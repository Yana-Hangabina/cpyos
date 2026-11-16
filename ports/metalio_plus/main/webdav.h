#pragma once

#include <esp_http_server.h>

// Read/write WebDAV subset over the SD card, registered on /* with wildcard URI
// matching. It shares the transfer path guards (no "..", no /.crosspoint).
namespace webdav {

bool registerHandlers(httpd_handle_t server);

}  // namespace webdav
