// Compile-time integration check for the SecureNet surface on the Plus port.
//
// SecureHttpClient is header-only and used by OTA, the browser, plugins and
// KOReader sync. This translation unit instantiates it so any API drift between
// the SDK header and the port's esp_tls-backed SecureClient fails the build.
// Never called: the linker drops it from the image.

#include <SecureHttpClient.h>

#include <string>

namespace port {
namespace net {

bool secureHttpSelfTest(const char* url) {
    freeink::SecureHttpClient http;
    http.setInsecure();
    http.setTimeout(8000);
    http.setFollowRedirects(true);
    http.addHeader("Accept", "text/html");
    if (!http.begin(url)) return false;
    const int code = http.GET();
    const std::string& body = http.getString();
    const bool ok = code > 0 && !body.empty();
    http.end();
    return ok;
}

}  // namespace net
}  // namespace port
