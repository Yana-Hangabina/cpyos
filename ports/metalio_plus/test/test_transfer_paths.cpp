#include <cstdio>
#include <string>
#include <vector>

#include "transfer_paths.h"

namespace {
int failures = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace web_transfer::paths;

    // Decoding
    expect(urlDecode("/Books/a%20b.epub") == "/Books/a b.epub", "decode space");
    expect(urlDecode("/%E4%B8%AD%E6%96%87.epub") == "/中文.epub", "decode UTF-8");
    expect(urlDecode("/a+b") == "/a b", "decode plus");
    expect(urlDecode("/a%2Fb") == "/a/b", "decode slash");
    expect(urlDecode("/%zz") == "/%zz", "bad percent kept");
    expect(urlDecode("/%2") == "/%2", "truncated percent kept");

    // Accepted logical paths
    for (const char* ok : {"/", "/Books", "/Books/a.epub", "/中文 名.epub", "/.fonts/x.ttf"}) {
        expect(safe(ok), std::string("accept ") + ok);
    }

    // Rejected: traversal, private dir, malformed
    for (const char* bad : {"", "Books", "/..", "/../etc/passwd", "/Books/../../secret", "//etc", "/a//b",
                            "/.crosspoint", "/.crosspoint/jjwxc/cookies", "/.crosspointfoo",
                            "/books/../.crosspoint/cookies"}) {
        expect(!safe(bad), std::string("reject ") + bad);
    }

    // Decoding happens before validation in the server: %2e%2e must not slip through
    expect(!safe(urlDecode("/%2e%2e/etc")), "encoded traversal rejected");
    expect(!safe(urlDecode("/.crosspoint%2Fcookies")), "encoded private dir rejected");
    expect(!safe("/.CROSSPOINT/cookies"), "uppercase private dir rejected");
    expect(!safe("/.CrOsSpOiNt"), "mixed-case private dir rejected");
    expect(!safe("/CROSSP~1/cookies"), "8.3 alias of the private dir rejected");
    expect(!safe("/crossp~1"), "8.3 alias, lowercase, rejected");
    expect(!safe("/CROSSPOI/jjwxc/cookies"), "collision-free 8.3 stem rejected");
    expect(safe("/Books/crosspoint-guide.epub"), "ordinary long name accepted");

    // Mapping stays under the SD root
    expect(toReal("/") == "/sdcard", "root maps");
    expect(toReal("/Books/a.epub") == "/sdcard/Books/a.epub", "child maps");

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("transfer paths: all checks passed\n");
    return 0;
}
