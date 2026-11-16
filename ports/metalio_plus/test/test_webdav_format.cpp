// Host tests for the WebDAV formatting/parsing helpers.
#include "webdav_format.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace webdav;

    check(xmlEscape("a<b&c\"d'e") == "a&lt;b&amp;c&quot;d&apos;e", "xml escape");
    check(hrefEncode("/Books/My Book.epub") == "/Books/My%20Book.epub", "href encodes space");
    check(hrefEncode("/a-b_c.d~e") == "/a-b_c.d~e", "href keeps unreserved");
    check(hrefEncode("/中文") == "/%E4%B8%AD%E6%96%87", "href encodes utf-8 bytes");

    std::string path;
    check(pathFromDestination("http://crosspoint.local/Books/a.epub", path) && path == "/Books/a.epub",
          "absolute URL");
    check(pathFromDestination("/Books/a.epub", path) && path == "/Books/a.epub", "path only");
    check(pathFromDestination("/Books/My%20Book.epub?x=1", path) && path == "/Books/My Book.epub",
          "decode + strip query");
    check(pathFromDestination("/Books/", path) && path == "/Books", "trailing slash trimmed");
    check(!pathFromDestination("Books/a.epub", path), "relative rejected");
    check(!pathFromDestination("http://host", path), "no path rejected");
    check(!pathFromDestination("/Books/../etc", path), "dotdot rejected");

    check(parseDepth(nullptr) == 1, "default depth");
    check(parseDepth("0") == 0, "depth 0");
    check(parseDepth("1") == 1, "depth 1");
    check(parseDepth("infinity") == 1, "depth infinity bounded");
    check(overwriteAllowed(nullptr), "overwrite default");
    check(overwriteAllowed("T"), "overwrite T");
    check(!overwriteAllowed("F"), "overwrite F");

    check(std::string(contentTypeFor("a.EPUB")) == "application/epub+zip", "epub type case-insensitive");
    check(std::string(contentTypeFor("a.bin")) == "application/octet-stream", "unknown type");

    std::time_t epoch = 0;
    check(httpDate(epoch) == "Thu, 01 Jan 1970 00:00:00 GMT", "http date");

    if (failures == 0) std::printf("webdav format: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
