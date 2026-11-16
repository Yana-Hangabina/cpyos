#pragma once

// Pure HID report decoding for the port's BLE keyboard host. Kept header-only
// and Arduino-free so a host test can pin the behaviour that otherwise would
// only be observable with a real remote.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BleKeyboardHost.h"

namespace ble_hid {

struct Edge {
    uint8_t usage = 0;
    uint8_t mods = 0;
};

inline char asciiForUsage(uint8_t usage, uint8_t mods) {
    const bool shift = (mods & 0x22) != 0;
    if (usage >= 0x04 && usage <= 0x1D) {
        char c = static_cast<char>('a' + (usage - 0x04));
        return shift ? static_cast<char>(c - 32) : c;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        static const char digits[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return shift ? shifted[usage - 0x1E] : digits[usage - 0x1E];
    }
    switch (usage) {
        case 0x2C: return ' ';
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default: return 0;
    }
}

inline freeink::SpecialKey specialForUsage(uint8_t usage) {
    switch (usage) {
        case 0x28: return freeink::SpecialKey::Enter;
        case 0x29: return freeink::SpecialKey::Escape;
        case 0x2A: return freeink::SpecialKey::Backspace;
        case 0x2B: return freeink::SpecialKey::Tab;
        case 0x4A: return freeink::SpecialKey::Home;
        case 0x4B: return freeink::SpecialKey::PageUp;
        case 0x4C: return freeink::SpecialKey::Delete;
        case 0x4D: return freeink::SpecialKey::End;
        case 0x4E: return freeink::SpecialKey::PageDown;
        case 0x4F: return freeink::SpecialKey::Right;
        case 0x50: return freeink::SpecialKey::Left;
        case 0x51: return freeink::SpecialKey::Down;
        case 0x52: return freeink::SpecialKey::Up;
        default: return freeink::SpecialKey::None;
    }
}

// Strips a Report ID prefix (9-byte keyboard, 3-byte consumer). Returns false
// when the frame is not a recognised keyboard/consumer shape.
inline bool stripReportId(const uint8_t*& data, size_t& len, size_t keyboardLen, size_t consumerLen) {
    if (len == keyboardLen + 1) {
        ++data;
        len = keyboardLen;
        return true;
    }
    if (len == consumerLen + 1) {
        ++data;
        len = consumerLen;
        return true;
    }
    return true;
}

// Boot keyboard reports: 8 bytes (modifiers, reserved, six usages). Emits one
// Edge per usage that was not present in `prev`, ignoring duplicates within the
// same frame (rollover). Updates `prev` to the current frame.
inline size_t decodeKeyboard(const uint8_t* data, size_t len, uint8_t prev[6], Edge* out, size_t cap) {
    if (data == nullptr || prev == nullptr || out == nullptr || len < 8) return 0;
    const uint8_t mods = data[0];
    size_t count = 0;
    for (size_t i = 0; i < 6; ++i) {
        const uint8_t usage = data[2 + i];
        if (usage == 0) continue;
        // Compare the SET of usages, not the slot: a pager may reorder its
        // report while a different key is pressed, which used to look like a
        // fresh press of the moved key.
        bool wasDown = false;
        for (size_t j = 0; j < 6; ++j) wasDown = wasDown || prev[j] == usage;
        if (wasDown) continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) duplicate = duplicate || data[2 + j] == usage;
        if (!duplicate && count < cap) {
            out[count++] = Edge{usage, mods};
        }
    }
    for (size_t i = 0; i < 6; ++i) prev[i] = data[2 + i];
    return count;
}

// Consumer control reports: 2-byte little-endian usage. Only the three usages
// the page-turner feature needs are reported.
inline bool decodeConsumer(const uint8_t* data, size_t len, Edge* out) {
    if (data == nullptr || out == nullptr || len < 2) return false;
    const uint16_t usage = static_cast<uint16_t>(data[0] | (data[1] << 8));
    switch (usage) {
        case 0x00B5: *out = Edge{0x00, 0}; return true;  // scan previous -> Left
        case 0x00B6: *out = Edge{0x01, 0}; return true;  // scan next     -> Right
        case 0x00CD: *out = Edge{0x02, 0}; return true;  // play/pause    -> Enter
        default: return false;
    }
}

}  // namespace ble_hid
