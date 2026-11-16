// Host tests for the WebSocket upload START parser (no ESP-IDF dependency).
#include "ws_protocol.h"

#include <cstdio>
#include <cstring>

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
    using ws_upload::parseStart;
    using ws_upload::safeFilename;
    ws_upload::Start start;

    check(parseStart("START:mybook.epub:1234567:/Books", start), "basic START");
    check(start.filename == "mybook.epub", "filename");
    check(start.size == 1234567, "size");
    check(start.path == "/Books", "path");

    check(parseStart("START:a.epub:0:", start) && start.path == "/", "empty path -> / and zero size");
    check(parseStart("START:a.epub:10:/A:B", start) && start.path == "/A:B", "colon in path");
    check(parseStart("START:a.epub:10:/", start) && start.path == "/", "root path");

    check(!parseStart("START:a.epub:10", start), "missing path field");
    check(!parseStart("START:a.epub::/Books", start), "empty size");
    check(!parseStart("START:a.epub:-1:/Books", start), "negative size");
    check(!parseStart("START:a.epub:12x:/Books", start), "non-digit size");
    check(!parseStart("START::10:/Books", start), "empty filename");
    check(!parseStart("HELLO", start), "wrong prefix");
    check(!parseStart("START:a.epub:999999999999:/Books", start), "overflow size");
    check(!parseStart("START:a.epub:536870913:/Books", start), "over 512 MiB");
    check(parseStart("START:a.epub:536870912:/Books", start), "exactly 512 MiB");
    check(!parseStart("START:a.epub:10:Books", start), "relative path");
    check(!parseStart("START:a.epub:10:/" + std::string(600, 'x'), start), "overlong path");

    check(safeFilename("book.epub"), "plain filename");
    check(!safeFilename(""), "empty filename");
    check(!safeFilename("."), "dot");
    check(!safeFilename(".."), "dotdot");
    check(!safeFilename(".hidden"), "leading dot");
    check(!safeFilename("a/b.epub"), "slash");
    check(!safeFilename("a\\b.epub"), "backslash");
    check(!safeFilename("a\nb"), "control char");
    check(!safeFilename(std::string(201, 'a')), "overlong filename");

    if (failures == 0) std::printf("ws protocol: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
