// Host regression for the PDFENG2 manifest: parsing and compatibility-class
// selection.  Pure text, ASan/UBSan.  One mutation run (disable the https-only
// check) must go red, proving the TLS rule is load-bearing.
#include "pdf/PdfEngineManifest.h"

#include <array>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;
void check(bool cond, const char* what) {
  ++checks;
  if (!cond) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

using pdf_engine::Entry;
using pdf_engine::kMaxEntries;
using pdf_engine::parseManifest;
using pdf_engine::selectForApp;

std::string hex64(char fill) { return std::string(64, fill); }

std::string line(const std::string& url, int size, char sha, uint32_t abi, uint32_t major,
                 uint32_t minor, uint32_t layout, uint32_t rev, char build) {
  char buf[320];
  std::snprintf(buf, sizeof(buf), "%s\t%d\t%s\t%u\t%u\t%u\t0x%08X\t%u\t%s\n", url.c_str(), size,
                hex64(sha).c_str(), abi, major, minor, layout, rev, hex64(build).c_str());
  return buf;
}

std::string manifestOf(const std::string& a, const std::string& b = "",
                       const std::string& c = "") {
  return std::string("PDFENG2\n") + a + b + c;
}

void testValidAndSelection() {
  const uint32_t layout = kPdfModuleLayoutKey;
  const std::string older =
      line("https://x/old.bin", 1000, 'a', kPdfModuleAbiVersion, kPdfModuleHostApiMajor,
           kPdfModuleHostApiMinor, layout, 7, 'b');
  const std::string newer =
      line("https://x/new.bin", 1000, 'c', kPdfModuleAbiVersion, kPdfModuleHostApiMajor,
           kPdfModuleHostApiMinor, layout, 9, 'd');
  std::array<Entry, kMaxEntries> entries{};
  size_t count = 0;
  check(parseManifest(manifestOf(older, newer), entries, count), "two matching entries parse");
  check(count == 2, "count is two");
  check(entries[0].size == 1000u && entries[0].engineRev == 7u, "fields decoded");
  check(entries[0].sha256[0] == 0xaa && entries[0].buildId[0] == 0xbb, "digests decoded");
  check(entries[0].hostApiMajor == kPdfModuleHostApiMajor, "host major decoded");
  check(entries[0].layoutKey == layout, "layout decoded");
  Entry picked;
  check(selectForApp(entries, count, picked), "a compatible entry is found");
  check(picked.url == "https://x/new.bin" && picked.engineRev == 9u, "the newest rev wins");
}

void testIgnoresOtherClasses() {
  const uint32_t layout = kPdfModuleLayoutKey;
  const std::string other_abi =
      line("https://x/abi.bin", 1000, 'a', kPdfModuleAbiVersion + 1, kPdfModuleHostApiMajor,
           kPdfModuleHostApiMinor, layout, 50, 'b');
  const std::string other_major =
      line("https://x/major.bin", 1000, 'a', kPdfModuleAbiVersion, kPdfModuleHostApiMajor + 1,
           kPdfModuleHostApiMinor, layout, 60, 'b');
  const std::string other_layout =
      line("https://x/layout.bin", 1000, 'a', kPdfModuleAbiVersion, kPdfModuleHostApiMajor,
           kPdfModuleHostApiMinor, layout ^ 1u, 70, 'b');
  std::array<Entry, kMaxEntries> entries{};
  size_t count = 0;
  check(parseManifest(manifestOf(other_abi, other_major, other_layout), entries, count),
        "foreign classes still parse");
  Entry picked;
  check(!selectForApp(entries, count, picked), "no compatible entry -> no download");
}

void testMinorRule() {
  const uint32_t layout = kPdfModuleLayoutKey;
  // Older engine (minor <= app) is fine; a newer host API minor is not.
  const std::string older =
      line("https://x/older.bin", 1000, 'a', kPdfModuleAbiVersion, kPdfModuleHostApiMajor,
           kPdfModuleHostApiMinor, layout, 1, 'b');
  std::array<Entry, kMaxEntries> entries{};
  size_t count = 0;
  check(parseManifest(manifestOf(older), entries, count), "older minor parses");
  Entry picked;
  check(selectForApp(entries, count, picked), "older minor accepted");

  const std::string newer =
      line("https://x/newer.bin", 1000, 'a', kPdfModuleAbiVersion, kPdfModuleHostApiMajor,
           kPdfModuleHostApiMinor + 1, layout, 1, 'b');
  check(parseManifest(manifestOf(newer), entries, count), "newer minor parses");
  check(!selectForApp(entries, count, picked), "newer minor refused");
}

void testRejects() {
  const std::string good =
      line("https://x/y", 1000, 'a', kPdfModuleAbiVersion, kPdfModuleHostApiMajor,
           kPdfModuleHostApiMinor, kPdfModuleLayoutKey, 1, 'b');
  std::array<Entry, kMaxEntries> entries{};
  size_t count = 0;
  check(!parseManifest(good, entries, count), "missing PDFENG2 header rejected");
  check(!parseManifest("PDFENG2\n", entries, count), "empty index rejected");
  check(!parseManifest("PDFENG1\n" + good, entries, count), "wrong header rejected");
  check(!parseManifest(manifestOf(good.substr(0, good.size() - 20)), entries, count),
        "truncated line rejected");
  check(!parseManifest(manifestOf(line("http://x/y", 1000, 'a', kPdfModuleAbiVersion,
                                      kPdfModuleHostApiMajor, kPdfModuleHostApiMinor,
                                      kPdfModuleLayoutKey, 1, 'b')),
                       entries, count),
        "http rejected (TLS only)");
  check(!parseManifest(manifestOf(line("https://x y", 1000, 'a', kPdfModuleAbiVersion,
                                      kPdfModuleHostApiMajor, kPdfModuleHostApiMinor,
                                      kPdfModuleLayoutKey, 1, 'b')),
                       entries, count),
        "space in url rejected");
  check(!parseManifest(manifestOf(line("https://x/y", 100, 'a', kPdfModuleAbiVersion,
                                      kPdfModuleHostApiMajor, kPdfModuleHostApiMinor,
                                      kPdfModuleLayoutKey, 1, 'b')),
                       entries, count),
        "undersize rejected");
  check(!parseManifest(manifestOf(line("https://x/y", 5000000, 'a', kPdfModuleAbiVersion,
                                      kPdfModuleHostApiMajor, kPdfModuleHostApiMinor,
                                      kPdfModuleLayoutKey, 1, 'b')),
                       entries, count),
        "oversize rejected");
  std::string bad_sha = good;
  bad_sha.replace(bad_sha.find(hex64('a')), 2, "zz");
  check(!parseManifest(manifestOf(bad_sha), entries, count), "non-hex sha rejected");
  std::string many = "PDFENG2\n";
  for (size_t i = 0; i <= kMaxEntries; ++i) many += good;
  check(!parseManifest(many, entries, count), "index larger than kMaxEntries rejected");
  // Comments and blank lines are allowed and do not count.
  check(parseManifest("# note\n\nPDFENG2\n" + good, entries, count), "comments tolerated before the header");
}

void testHexRoundtrip() {
  std::array<uint8_t, 32> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 7);
  const std::string hex = pdf_engine::hex32Lower(bytes);
  std::array<uint8_t, 32> back{};
  check(hex.size() == 64, "hex32Lower length");
  check(pdf_engine::parseHex32(hex, back) && back == bytes, "hex roundtrip");
}

}  // namespace

int main() {
  testValidAndSelection();
  testIgnoresOtherClasses();
  testMinorRule();
  testRejects();
  testHexRoundtrip();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
