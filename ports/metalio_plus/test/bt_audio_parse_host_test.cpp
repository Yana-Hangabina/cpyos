#include <cstdio>
#include <cstring>

#include "plus_bt_audio_parse.h"

namespace {
int failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}
}  // namespace

int main() {
    char addr[13];
    char name[64];
    {
        expect(plus_bt_audio::parseDeviceLine("AT+BT:AABBCCDDEEFF耳机", addr, sizeof(addr), name, sizeof(name)),
               "name line parses");
        expect(std::strcmp(addr, "AABBCCDDEEFF") == 0, "address is 12 hex chars");
        expect(std::strcmp(name, "耳机") == 0, "UTF-8 name kept");
    }
    {
        expect(plus_bt_audio::parseDeviceLine("AT+BT:aabbccddeeff", addr, sizeof(addr), name, sizeof(name)),
               "lowercase address parses");
        expect(std::strcmp(addr, "aabbccddeeff") == 0 && name[0] == '\0', "empty name ok");
    }
    {
        expect(!plus_bt_audio::parseDeviceLine("AT+BT:AABBCCDDEEF", addr, sizeof(addr), name, sizeof(name)),
               "short address rejected");
        expect(!plus_bt_audio::parseDeviceLine("AT+BT:GGGGCCDDEEFF", addr, sizeof(addr), name, sizeof(name)),
               "non-hex rejected");
        expect(!plus_bt_audio::parseDeviceLine("AT+BT:AABBCCDDEEFF", nullptr, sizeof(addr), name, sizeof(name)),
               "null address out rejected");
        expect(!plus_bt_audio::parseDeviceLine(nullptr, addr, sizeof(addr), name, sizeof(name)),
               "null line rejected");
        // A tiny name buffer must truncate, not overflow.
        char tiny[4];
        expect(plus_bt_audio::parseDeviceLine("AT+BT:AABBCCDDEEFFabcdef", addr, sizeof(addr), tiny, sizeof(tiny)),
               "tiny name buffer still parses");
        expect(std::strlen(tiny) == 3, "name truncated to cap-1");
    }
    expect(plus_bt_audio::isInquiryComplete("xx INQ COMPLETE yy"), "inquiry complete");
    expect(plus_bt_audio::isConnectSuccess("CONNECT SUCCESS"), "connect success");
    expect(plus_bt_audio::isConnectTimeout("CONNECT TIMEOUT"), "connect timeout");
    expect(!plus_bt_audio::isConnectSuccess("CONNECT TIMEOUT"), "timeout is not success");
    expect(!plus_bt_audio::isInquiryComplete(nullptr), "null status rejected");

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("bt audio parse: all checks passed\n");
    return 0;
}
