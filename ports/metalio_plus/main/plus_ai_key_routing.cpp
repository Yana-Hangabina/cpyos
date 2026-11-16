#include "plus_ai_key_routing.h"

#include <cstring>

namespace plus_ai_key {
namespace {
constexpr const char* kBlocked[] = {
    "Boot",          "Sleep",         "Standby",      "WifiSelection", "KeyboardEntry",
    "OtaUpdate",     "UsbDrive",      "PlusTransfer", "PlusOta",       "PlusCamera",
    "PowerOff",      "PlusVoiceMemo", "PlusBtAudio",
};
}  // namespace

bool routingBlocked(const char* activityName) {
    if (activityName == nullptr || *activityName == '\0') return true;
    for (const char* name : kBlocked) {
        if (std::strcmp(activityName, name) == 0) return true;
    }
    return false;
}

}  // namespace plus_ai_key
