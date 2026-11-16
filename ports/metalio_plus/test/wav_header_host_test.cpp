#include <cstdio>
#include <cstring>

#include "plus_wav.h"

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
    uint8_t h[plus_wav::kHeaderBytes] = {};
    plus_wav::writeHeader(h, 0, 16000);
    expect(std::memcmp(h, "RIFF", 4) == 0, "RIFF magic");
    expect(plus_wav::get32(h + 4) == 36, "empty RIFF size");
    expect(std::memcmp(h + 8, "WAVEfmt ", 8) == 0, "WAVE fmt magic");
    expect(plus_wav::get32(h + 16) == 16, "fmt chunk size");
    expect(h[20] == 1 && h[21] == 0, "PCM format");
    expect(h[22] == 1 && h[23] == 0, "mono");
    expect(plus_wav::get32(h + 24) == 16000, "sample rate");
    expect(plus_wav::get32(h + 28) == 32000, "byte rate");
    expect(h[32] == 2 && h[34] == 16, "block align / bits");
    expect(std::memcmp(h + 36, "data", 4) == 0, "data magic");
    expect(plus_wav::get32(h + 40) == 0, "empty data size");

    plus_wav::writeHeader(h, 96000, 22050);
    expect(plus_wav::get32(h + 4) == 36 + 96000, "patched RIFF size");
    expect(plus_wav::get32(h + 24) == 22050, "patched rate");
    expect(plus_wav::get32(h + 28) == 44100, "patched byte rate");
    expect(plus_wav::get32(h + 40) == 96000, "patched data size");

    // Little-endian helpers round-trip the full 32-bit range.
    uint8_t b[4] = {};
    plus_wav::put32(b, 0x89ABCDEFu);
    expect(plus_wav::get32(b) == 0x89ABCDEFu, "32-bit little-endian");
    expect(plus_wav::kHeaderBytes == 44, "header is 44 bytes");

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("wav header: all checks passed\n");
    return 0;
}
