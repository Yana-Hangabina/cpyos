// Host check for the sleep-cover scaling pass:
//   - BmpToBmpConverter upscales a small image to fill the panel (portrait dims),
//   - downscales a large one while preserving aspect (fit) or cropping to fill,
//   - keeps a 1-bit source 1-bit so B/W images stay on the B/W refresh path,
//   - emits a self-consistent BMP header/size, and honours top-down sources.
// Bitmap::readRowGray() is exercised indirectly (the converter's only row reader).
//
// Run: bash test/bmp_to_bmp/run_bmp_to_bmp_test.sh

#include <BmpToBmpConverter.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Print.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Defined by the stub HalDisplay.h; the converter sizes against it (120x200 portrait).
HalDisplay display;

namespace {
int failures = 0;

void check(const bool condition, const char* what) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

class Output final : public Print {
 public:
  std::vector<uint8_t> bytes;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    bytes.insert(bytes.end(), data, data + count);
    return count;
  }
};

uint32_t le32(const std::vector<uint8_t>& b, const size_t at) {
  return uint32_t{b[at]} | uint32_t{b[at + 1]} << 8 | uint32_t{b[at + 2]} << 16 | uint32_t{b[at + 3]} << 24;
}

void put32(std::vector<uint8_t>& b, const uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// 24bpp or 1bpp BMP whose top (or left) half is black and the other half white.
std::vector<uint8_t> splitBmp(const int width, const int height, const bool topDown, const bool oneBit,
                              const bool leftRight = false) {
  const int bits = oneBit ? 1 : 24;
  const int rowBytes = (width * bits + 31) / 32 * 4;
  const uint32_t offset = 54 + (oneBit ? 8 : 0);
  std::vector<uint8_t> b = {'B', 'M'};
  put32(b, offset + rowBytes * height);
  put32(b, 0);
  put32(b, offset);
  put32(b, 40);
  put32(b, width);
  put32(b, topDown ? static_cast<uint32_t>(-height) : height);
  b.insert(b.end(), {1, 0, static_cast<uint8_t>(bits), 0});
  for (int i = 0; i < 6; ++i) put32(b, i == 4 && oneBit ? 2 : 0);
  if (oneBit) {
    put32(b, 0);
    put32(b, 0xFFFFFF);
  }
  for (int i = 0; i < height; ++i) {
    const int y = topDown ? i : height - 1 - i;
    if (!leftRight) {
      b.insert(b.end(), rowBytes, y < height / 2 ? 0 : 0xFF);
      continue;
    }
    for (int x = 0; x < rowBytes; ++x) b.push_back(x < rowBytes / 2 ? 0 : 0xFF);
  }
  return b;
}

std::string path;  // one temp file reused by every case

Output convert(const std::vector<uint8_t>& bmp, const bool crop) {
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
  }
  HalFile file;
  check(file.open(path.c_str(), "rb"), "open source BMP");
  Output out;
  check(BmpToBmpConverter::bmpFileToBmpStream(file, out, crop, false), "convert");
  file.close();
  return out;
}

// Level of output pixel (x, y) top-down: 0..3 for a 2-bit BMP, 0/1 for a 1-bit one.
int level(const Output& out, int x, int y) {
  const unsigned bits = out.bytes[28];
  const uint32_t width = le32(out.bytes, 18);
  const auto rawHeight = static_cast<int32_t>(le32(out.bytes, 22));
  if (rawHeight > 0) y = rawHeight - 1 - y;
  const size_t row = le32(out.bytes, 10) + static_cast<size_t>(y) * ((width * bits + 31) / 32 * 4);
  const uint8_t byte = out.bytes[row + static_cast<size_t>(x) * bits / 8];
  return bits == 1 ? (byte >> (7 - x % 8)) & 1 : (byte >> (6 - (x % 4) * 2)) & 3;
}

void upsizesSmallImageToFillScreen() {
  for (const bool topDown : {false, true}) {
    const Output out = convert(splitBmp(10, 16, topDown, false), true);
    check(le32(out.bytes, 18) == 120u, "upscale: portrait width");
    check(le32(out.bytes, 22) == (topDown ? static_cast<uint32_t>(-200) : 200u), "upscale: portrait height");
    check(out.bytes[28] == 2, "upscale: 24bpp source becomes 2-bit");
    check(out.bytes.size() == le32(out.bytes, 2), "upscale: header size matches payload");
    check(level(out, 60, 2) == 0, "upscale: top stays black");
    check(level(out, 60, 197) == 3, "upscale: bottom stays white");
  }
}

// A nearest/box "upscale" copies whole source rows, so a two-tone source can only
// ever produce the two extreme levels. Bilinear + one Atkinson pass must leave
// intermediate levels where the two source tones meet (in either fit or crop).
void upscaleInterpolatesBetweenSourceRows() {
  for (const bool crop : {false, true}) {
    const Output out = convert(splitBmp(2, 8, true, false), crop);
    const uint32_t width = le32(out.bytes, 18);
    const int height = std::abs(static_cast<int32_t>(le32(out.bytes, 22)));
    bool sawIntermediate = false;
    for (int y = 0; y < height && !sawIntermediate; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const int value = level(out, static_cast<int>(x), y);
        sawIntermediate = value == 1 || value == 2;
        if (sawIntermediate) break;
      }
    }
    check(sawIntermediate, crop ? "upscale(crop): tones are interpolated"
                                : "upscale(fit): tones are interpolated");
  }
}

void downscalesAndFits() {
  const Output out = convert(splitBmp(160, 128, false, false), false);
  check(le32(out.bytes, 18) == 120u, "fit: width limited by panel");
  check(le32(out.bytes, 22) == 96u, "fit: height keeps the 160x128 aspect");
  check(level(out, 60, 1) == 0, "fit: top stays black");
  check(level(out, 60, 94) == 3, "fit: bottom stays white");
}

void cropsCenterWhileDownscaling() {
  for (const bool topDown : {false, true}) {
    const Output out = convert(splitBmp(320, 128, topDown, false, true), true);
    check(le32(out.bytes, 18) == 120u, "crop: fills the panel width");
    check(le32(out.bytes, 22) == (topDown ? static_cast<uint32_t>(-200) : 200u), "crop: fills the panel height");
    check(level(out, 2, 100) == 0, "crop: left edge is the black half");
    check(level(out, 117, 100) == 3, "crop: right edge is the white half");
  }
}

void oneBitSourceStaysOneBit() {
  const Output out = convert(splitBmp(20, 32, false, true), true);
  check(out.bytes[28] == 1, "1-bit: source stays 1-bit");
  check(le32(out.bytes, 18) == 120u, "1-bit: upscaled width");
  check(le32(out.bytes, 22) == 200u, "1-bit: upscaled height");
  check(level(out, 3, 2) == 0, "1-bit: top stays black");
  check(level(out, 3, 197) == 1, "1-bit: bottom stays white");
}

}  // namespace

int main() {
  path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/bmp_to_bmp_test.bmp";
  upsizesSmallImageToFillScreen();
  upscaleInterpolatesBetweenSourceRows();
  downscalesAndFits();
  cropsCenterWhileDownscaling();
  oneBitSourceStaysOneBit();
  std::remove(path.c_str());
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("bmp_to_bmp converter: upscale/fit/crop/1-bit/header checks passed\n");
  return 0;
}
