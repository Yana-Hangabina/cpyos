// Host check for static GIF support:
//   - GifCommon::calculateLayout (cover/thumbnail geometry, invalid targets)
//   - GifLimitedWriter (stops an extraction at the byte cap)
//   - GifToBmpConverter, compiled against the real pinned AnimatedGIF, on a
//     small Pillow-encoded GIF fixture: 2-bit cover, 8-bit thumbnail, 1-bit,
//     short-write rejection, interlaced rejection and bad LZW code size.
//
// Run: bash test/gif_support/run_gif_support_test.sh

#include <GifImageLayout.h>
#include <GifLimitedWriter.h>
#include <GifToBmpConverter.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <ImageDimsProbe.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Defined by the stub HalDisplay.h; the cover entry point sizes against it.
HalDisplay display;

namespace {
int failures = 0;

void check(const bool condition, const char* what) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// Original Pillow-encoded 8x4 gray pattern; palette entry 3 is transparent.
std::vector<uint8_t> image() {
  const std::string hex =
      "47494638396108000400810000000000555555aaaaaaffffff21f90401000003002c000000000800040000081500010410304020c181"
      "051102206830614286030302003b";
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

class Output final : public Print {
 public:
  std::vector<uint8_t> bytes;
  size_t limit = SIZE_MAX;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    count = std::min(count, limit - bytes.size());
    bytes.insert(bytes.end(), data, data + count);
    return count;
  }
};

uint32_t le32(const std::vector<uint8_t>& bytes, size_t at) {
  return uint32_t{bytes[at]} | uint32_t{bytes[at + 1]} << 8 | uint32_t{bytes[at + 2]} << 16 |
         uint32_t{bytes[at + 3]} << 24;
}

void writeFixture(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void checkBmp(const Output& out, const uint32_t width, const uint32_t height, const unsigned bits,
              const char* what) {
  if (out.bytes.size() < 62 || out.bytes[0] != 'B' || out.bytes[1] != 'M') {
    fprintf(stderr, "FAIL: %s (no BMP header)\n", what);
    ++failures;
    return;
  }
  const uint32_t headerBytes = 54 + 4u * (1u << bits);
  const uint32_t imageBytes = (width * bits + 31) / 32 * 4 * height;
  check(le32(out.bytes, 2) == out.bytes.size(), what);
  check(le32(out.bytes, 10) == headerBytes, what);
  check(le32(out.bytes, 18) == width, what);
  check(le32(out.bytes, 22) == uint32_t{0} - height, what);
  check(out.bytes[28] == bits, what);
  check(le32(out.bytes, 34) == imageBytes, what);
  check(out.bytes.size() == headerBytes + imageBytes, what);
}

void testLayout() {
  const std::pair<int, int> dimensionsCases[] = {{1, 3072}, {3072, 1}, {640, 480}};
  const std::pair<int, int> targets[] = {{480, 800}, {800, 480}};
  for (const auto& dimensions : dimensionsCases) {
    for (const auto& target : targets) {
      GifCommon::ImageLayout layout;
      check(GifCommon::calculateLayout(dimensions.first, dimensions.second, target.first, target.second, true, layout),
            "crop layout accepted");
      check(layout.width == target.first && layout.height == target.second, "crop fills the target box");
      check(layout.sourceX >= 0 && layout.sourceY >= 0, "crop origin is non-negative");
      check(layout.sourceX + layout.sourceWidth <= dimensions.first, "crop stays inside the source width");
      check(layout.sourceY + layout.sourceHeight <= dimensions.second, "crop stays inside the source height");
    }
  }

  GifCommon::ImageLayout fit;
  check(GifCommon::calculateLayout(640, 480, 240, 400, false, fit), "fit layout accepted");
  check(fit.width == 240 && fit.height == 180, "fit keeps the aspect ratio");
  check(fit.sourceWidth == 640 && fit.sourceHeight == 480, "fit keeps the whole source");

  GifCommon::ImageLayout invalid;
  check(!GifCommon::calculateLayout(1, 1, 0, 800, true, invalid), "zero target width rejected");
  check(!GifCommon::calculateLayout(1, 1, 480, -1, true, invalid), "negative target height rejected");
  check(!GifCommon::calculateLayout(1, 1, 480, 32767, true, invalid), "oversized target rejected");
}

class CountingOutput final : public Print {
 public:
  size_t total = 0;
  size_t write(uint8_t) override {
    ++total;
    return 1;
  }
  size_t write(const uint8_t*, size_t size) override {
    total += size;
    return size;
  }
};

void testLimitedWriter() {
  CountingOutput output;
  GifLimitedWriter writer(output, 10);
  constexpr uint8_t bytes[8] = {};
  check(writer.write(bytes, 8) == 8u, "limited writer passes bytes under the cap");
  check(writer.write(bytes, 8) == 0u, "limited writer refuses a chunk that would cross the cap");
  check(writer.write(bytes, 2) == 2u, "limited writer fills up to the cap");
  check(writer.write(0) == 0u, "limited writer refuses past the cap");
  check(output.total == 10u, "limited writer wrote exactly the cap");
}

void testConversions(const std::string& path) {
  HalFile file;
  check(file.open(path.c_str(), "rb"), "fixture opens");

  {
    Output out;
    check(GifToBmpConverter::gifFileToBmpStream(file, out, /*crop=*/true), "2-bit cover conversion");
    // Cover target is the panel size with swapped axes (120x200), cropped.
    checkBmp(out, 120, 200, 2, "2-bit cover BMP layout");
  }

  {
    Output out;
    check(GifToBmpConverter::gifFileToBmpStreamWithSize(file, out, 32, 16, true,
                                                        GifToBmpConverter::Output::Gray8),
          "8-bit thumbnail conversion");
    checkBmp(out, 32, 16, 8, "8-bit thumbnail BMP layout");
    // Transparency (palette entry 3) is composited on white, so no sample may
    // stay fully transparent: with an 8-bit palette the row must contain 255.
    bool foundWhite = false;
    for (size_t i = le32(out.bytes, 10); i < out.bytes.size(); ++i) foundWhite |= out.bytes[i] == 255;
    check(foundWhite, "transparent GIF pixels are composited on white");
  }

  {
    Output out;
    check(GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(file, out, 8, 4), "1-bit conversion");
    checkBmp(out, 8, 4, 1, "1-bit BMP layout");
  }

  // Short output writes must fail the conversion rather than publish a cut BMP.
  for (size_t limit = 0; limit <= 70; ++limit) {
    Output out;
    out.limit = limit;
    check(!GifToBmpConverter::gifFileToBmpStream(file, out, true), "short BMP writes are rejected");
    check(out.bytes.size() == limit, "short BMP writes stop at the limit");
  }
  check(file.isOpen(), "the source file is left open after a rejected conversion");
}

void testDimsProbe() {
  // The content probe is what lets the EPUB parser accept a GIF whose href has
  // no extension: it must stamp the format only after the whole signature.
  const auto gif = image();
  ImageDimsProbe probe;
  probe.write(gif.data(), gif.size());
  ImageDimensions dims = {0, 0};
  check(probe.detectedFormat() == ImageDimsProbe::Format::Gif, "GIF signature is detected from content");
  check(probe.getDimensions(dims), "GIF dimensions are read");
  check(dims.width == 8 && dims.height == 4, "GIF logical screen size");
  check(std::string(ImageDimsProbe::extensionForFormat(ImageDimsProbe::Format::Gif)) == ".gif",
        "GIF maps to the .gif extension");

  ImageDimsProbe png;
  const uint8_t pngHeader[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13,
                                 'I',  'H', 'D', 'R', 0,    0,     0,    16,    0, 0, 0, 32};
  png.write(pngHeader, sizeof(pngHeader));
  check(png.detectedFormat() == ImageDimsProbe::Format::Png, "PNG still detected");
  check(png.getDimensions(dims) && dims.width == 16 && dims.height == 32, "PNG dimensions still read");
}

void testRejections(const std::string& path) {
  for (const bool interlaced : {false, true}) {
    auto bytes = image();
    if (interlaced)
      bytes[42] |= 0x40;  // image descriptor packed field: interlace flag
    else
      bytes[43] = 13;  // LZW minimum code size out of range
    writeFixture(path, bytes);
    HalFile file;
    check(file.open(path.c_str(), "rb"), "rejection fixture opens");
    Output out;
    check(!GifToBmpConverter::gifFileToBmpStream(file, out, true), "interlaced / bad code size rejected");
    check(out.bytes.empty(), "nothing is written for a rejected GIF");
  }
}
}  // namespace

int main() {
  testLayout();
  testLimitedWriter();

  const std::string path = "/tmp/crossmux-gif-support-test.gif";
  writeFixture(path, image());
  testConversions(path);
  testDimsProbe();
  testRejections(path);
  std::remove(path.c_str());

  if (failures != 0) {
    fprintf(stderr, "%d GIF support check(s) failed\n", failures);
    return 1;
  }
  printf("GIF support: all checks passed\n");
  return 0;
}
