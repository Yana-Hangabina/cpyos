#include <cstdio>
#include <cstring>

#include "BleKeyMapping.h"
#include "ble_hid_decode.h"

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
    using namespace ble_hid;
    using freeink::SpecialKey;

    // ASCII mapping + modifiers
    expect(asciiForUsage(0x04, 0) == 'a', "a");
    expect(asciiForUsage(0x04, 0x02) == 'A', "shift a");
    expect(asciiForUsage(0x1E, 0) == '1', "1");
    expect(asciiForUsage(0x1E, 0x02) == '!', "shift 1");
    expect(asciiForUsage(0x2C, 0) == ' ', "space");
    expect(asciiForUsage(0x2D, 0) == '-', "-");
    expect(asciiForUsage(0x2D, 0x02) == '_', "_");
    expect(asciiForUsage(0x38, 0x02) == '?', "?");
    expect(asciiForUsage(0x00, 0) == 0, "none");

    // Specials
    expect(specialForUsage(0x28) == SpecialKey::Enter, "enter");
    expect(specialForUsage(0x4F) == SpecialKey::Right, "right");
    expect(specialForUsage(0x50) == SpecialKey::Left, "left");
    expect(specialForUsage(0x4E) == SpecialKey::PageDown, "pagedown");
    expect(specialForUsage(0x04) == SpecialKey::None, "letter has no special");

    // Keyboard edges: new key, held key, replacement, rollover duplicate
    {
        uint8_t prev[6] = {};
        Edge out[6];
        const uint8_t press[8] = {0, 0, 0x04, 0, 0, 0, 0, 0};
        expect(decodeKeyboard(press, 8, prev, out, 6) == 1 && out[0].usage == 0x04, "first press");
        expect(decodeKeyboard(press, 8, prev, out, 6) == 0, "held key does not repeat");
        const uint8_t next[8] = {0x02, 0, 0x05, 0, 0, 0, 0, 0};
        expect(decodeKeyboard(next, 8, prev, out, 6) == 1 && out[0].usage == 0x05 && out[0].mods == 0x02,
               "replacement key with shift");
        const uint8_t rollover[8] = {0, 0, 0x06, 0x06, 0, 0, 0, 0};
        expect(decodeKeyboard(rollover, 8, prev, out, 6) == 1 && out[0].usage == 0x06, "rollover dedup");
        const uint8_t release[8] = {};
        expect(decodeKeyboard(release, 8, prev, out, 6) == 0, "release has no edge");
    }

    // Report ID prefix: 9-byte keyboard frame / 3-byte consumer frame
    {
        const uint8_t input[9] = {1, 0, 0, 0x04, 0, 0, 0, 0, 0};
        const uint8_t* data = input;
        size_t len = sizeof(input);
        stripReportId(data, len, 8, 2);
        uint8_t prev[6] = {};
        Edge out[6];
        expect(len == 8 && decodeKeyboard(data, len, prev, out, 6) == 1 && out[0].usage == 0x04,
               "keyboard report id skipped");
        const uint8_t consumer[3] = {2, 0xB6, 0x00};
        data = consumer;
        len = sizeof(consumer);
        stripReportId(data, len, 8, 2);
        Edge edge;
        expect(len == 2 && decodeConsumer(data, len, &edge) && edge.usage == 1, "consumer report id skipped");
    }

    // A reordered report with the same set of keys is not a new press.
    {
        uint8_t prev[6] = {0x04, 0x05, 0, 0, 0, 0};
        Edge out[6];
        const uint8_t reordered[8] = {0, 0, 0x05, 0x04, 0, 0, 0, 0};
        expect(decodeKeyboard(reordered, 8, prev, out, 6) == 0, "reordered set has no edge");
        const uint8_t added[8] = {0, 0, 0x06, 0x04, 0x05, 0, 0, 0};
        expect(decodeKeyboard(added, 8, prev, out, 6) == 1 && out[0].usage == 0x06,
               "only the new usage is an edge");
    }

    // Consumer page-turn usages
    {
        Edge edge;
        expect(decodeConsumer(reinterpret_cast<const uint8_t*>("\xB5\x00"), 2, &edge) && edge.usage == 0, "scan previous");
        expect(decodeConsumer(reinterpret_cast<const uint8_t*>("\xB6\x00"), 2, &edge) && edge.usage == 1, "scan next");
        expect(decodeConsumer(reinterpret_cast<const uint8_t*>("\xCD\x00"), 2, &edge) && edge.usage == 2, "play/pause");
        expect(!decodeConsumer(reinterpret_cast<const uint8_t*>("\x00\x00"), 2, &edge), "release");
    }

    // A fresh device pages with the standard arrow/media keys without a
    // learn step; the values mirror freeink::SpecialKey (Left/Right/PageUp/PageDown).
    {
        const bleinput::KeyMap map = bleinput::defaultMap();
        bleinput::Action action = bleinput::Action::Count;
        expect(bleinput::lookup(map, 0, 6, action) && action == bleinput::Action::PageBack,
               "default Left -> PageBack");
        expect(bleinput::lookup(map, 0, 7, action) && action == bleinput::Action::PageForward,
               "default Right -> PageForward");
        expect(bleinput::lookup(map, 0, 11, action) && action == bleinput::Action::PageBack,
               "default PageUp -> PageBack");
        expect(bleinput::lookup(map, 0, 12, action) && action == bleinput::Action::PageForward,
               "default PageDown -> PageForward");
        expect(!bleinput::lookup(map, 0, 1, action), "Enter is not a page turn");
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ble hid decode: all checks passed\n");
    return 0;
}
