// Host test for the custom HTTP header line parser/formatter (upstream 65e3f9d0).
// Pure logic: no HAL, no Arduino, no network.
#include <HttpHeader.h>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void expectEq(const char* what, const std::string& got, const std::string& want) {
  if (got == want) return;
  std::fprintf(stderr, "FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
  g_failures++;
}

void expectParsed(const char* line, const char* wantName, const char* wantValue) {
  HttpHeader header;
  // Pre-fill so a parser that leaves fields untouched is caught.
  header.name = "stale";
  header.value = "stale";
  parseHttpHeaderLine(line, header);
  expectEq(line, header.name, wantName);
  expectEq(line, header.value, wantValue);
}

}  // namespace

int main() {
  expectParsed("CF-Access-Client-Id: abc123", "CF-Access-Client-Id", "abc123");
  expectParsed("  X-Token :  spaced value  ", "X-Token", "spaced value");
  expectParsed("Authorization:\tBearer x.y.z", "Authorization", "Bearer x.y.z");
  // Colons inside the value (URLs, timestamps) stay in the value.
  expectParsed("X-Origin: https://example.com:8443/a", "X-Origin", "https://example.com:8443/a");
  // A colon-less entry is a bare name with no value.
  expectParsed("X-Flag", "X-Flag", "");
  expectParsed("   ", "", "");
  expectParsed("", "", "");
  // Empty value after the colon.
  expectParsed("X-Empty:    ", "X-Empty", "");

  HttpHeader header{"CF-Access-Client-Secret", "s3cret"};
  expectEq("format", formatHttpHeaderLine(header), "CF-Access-Client-Secret: s3cret");
  expectEq("format masked", formatHttpHeaderLine(header, true), "CF-Access-Client-Secret: ******");
  expectEq("format name only", formatHttpHeaderLine(HttpHeader{"X-Flag", ""}), "X-Flag");
  expectEq("format unset", formatHttpHeaderLine(HttpHeader{}), "");
  // A masked display must never leak the value.
  if (formatHttpHeaderLine(header, true).find("s3cret") != std::string::npos) {
    std::fprintf(stderr, "FAIL masked format leaked the value\n");
    g_failures++;
  }

  // Round-trip: parse(format(h)) == h for a set header.
  HttpHeader round;
  parseHttpHeaderLine(formatHttpHeaderLine(header), round);
  expectEq("roundtrip name", round.name, header.name);
  expectEq("roundtrip value", round.value, header.value);

  if (g_failures != 0) {
    std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
    return 1;
  }
  std::printf("HttpHeader host test: all assertions passed\n");
  return 0;
}
