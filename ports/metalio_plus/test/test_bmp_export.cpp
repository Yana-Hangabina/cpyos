// Host tests for the 1-bit framebuffer -> BMP encoder.
#include "bmp_export.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
uint32_t get32(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
uint16_t get16(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}
}  // namespace

int main() {
    check(bmp_export::rowBytes(1216) == 152, "1216 row bytes");
    check(bmp_export::paddedRowBytes(1216) == 152, "1216 padded row");
    check(bmp_export::paddedRowBytes(10) == 4, "10 px pads to 4");
    check(bmp_export::paddedRowBytes(17) == 4, "17 px pads to 4");
    check(bmp_export::fileSize(1216, 684) == 14 + 40 + 8 + 152 * 684, "file size");

    // 10x2 image: row 0 = 0b10100000 0b01000000, row 1 = all white.
    const uint8_t fb[4] = {0xA0, 0x40, 0xFF, 0xFF};
    std::vector<uint8_t> out;
    check(bmp_export::writeMonoBmp(fb, 2, 10, 2, false, out), "encode");
    check(out.size() == bmp_export::fileSize(10, 2), "encoded size");
    check(out[0] == 'B' && out[1] == 'M', "magic");
    check(get32(out, 2) == out.size(), "file size field");
    check(get32(out, 10) == 62, "pixel offset");
    check(get32(out, 14) == 40, "header size");
    check(get32(out, 18) == 10, "width");
    check(static_cast<int32_t>(get32(out, 22)) == -2, "negative height (top-down)");
    check(get16(out, 26) == 1 && get16(out, 28) == 1, "1 bpp");
    check(get32(out, 34) == 4 * 2, "pixel byte count");
    check(get32(out, 46) == 2, "palette size");
    check(out[62 + 0] == 0xA0 && out[62 + 1] == 0x40 && out[62 + 2] == 0 && out[62 + 3] == 0,
          "row 0 bits + padding");
    check(out[62 + 4] == 0xFF && out[62 + 5] == 0xFF, "row 1 bits");

    std::vector<uint8_t> inverted;
    check(bmp_export::writeMonoBmp(fb, 2, 10, 2, true, inverted), "invert");
    check(inverted[62] == 0x5F && inverted[62 + 1] == 0xBF, "inverted bits");

    check(!bmp_export::writeMonoBmp(nullptr, 2, 10, 2, false, out), "null rejected");
    check(!bmp_export::writeMonoBmp(fb, 0, 10, 2, false, out), "zero stride rejected");

    if (failures == 0) std::printf("bmp export: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
