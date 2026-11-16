#include <cstdio>

#include "plus_ai_key_routing.h"

namespace {
int failures = 0;
void expectBlocked(const char* name, bool blocked) {
    if (plus_ai_key::routingBlocked(name) != blocked) {
        std::printf("FAIL %s should be %s\n", name, blocked ? "blocked" : "allowed");
        ++failures;
    }
}
}  // namespace

int main() {
    const char* blocked[] = {"Boot",      "Sleep",      "Standby",       "WifiSelection", "KeyboardEntry",
                             "OtaUpdate", "UsbDrive",   "PlusTransfer",  "PlusOta",       "PlusCamera",
                             "PowerOff",  "PlusVoiceMemo", "PlusBtAudio"};
    for (const char* name : blocked) expectBlocked(name, true);
    expectBlocked(nullptr, true);
    expectBlocked("", true);
    const char* allowed[] = {"InxRecent", "LibraryList", "EpubReader", "TxtReader", "Settings",
                             "My",        "Jjwxc",       "WebBrowser", "PlusVoice", "FileBrowser"};
    for (const char* name : allowed) expectBlocked(name, false);
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ai key routing: all checks passed\n");
    return 0;
}
