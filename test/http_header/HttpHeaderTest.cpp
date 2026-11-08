#include <gtest/gtest.h>

#include <HttpHeader.h>

TEST(HttpHeader, ParsesNameAndValue) {
  HttpHeader header;
  parseHttpHeaderLine("CF-Access-Client-Id: abc123", header);
  EXPECT_EQ(header.name, "CF-Access-Client-Id");
  EXPECT_EQ(header.value, "abc123");
}

TEST(HttpHeader, TrimsWhitespaceAroundBothFields) {
  HttpHeader header;
  parseHttpHeaderLine("  X-Token :\tspaced value  ", header);
  EXPECT_EQ(header.name, "X-Token");
  EXPECT_EQ(header.value, "spaced value");
}

TEST(HttpHeader, KeepsColonsInsideTheValue) {
  HttpHeader header;
  parseHttpHeaderLine("X-Origin: https://example.com:8443/a", header);
  EXPECT_EQ(header.value, "https://example.com:8443/a");
}

TEST(HttpHeader, MissingColonIsABareName) {
  HttpHeader header;
  parseHttpHeaderLine("X-Flag", header);
  EXPECT_EQ(header.name, "X-Flag");
  EXPECT_EQ(header.value, "");
}

TEST(HttpHeader, FormatsAndMasks) {
  const HttpHeader header{"CF-Access-Client-Secret", "s3cret"};
  EXPECT_EQ(formatHttpHeaderLine(header), "CF-Access-Client-Secret: s3cret");
  EXPECT_EQ(formatHttpHeaderLine(header, true), "CF-Access-Client-Secret: ******");
  EXPECT_EQ(formatHttpHeaderLine(HttpHeader{"X-Flag", ""}), "X-Flag");
  EXPECT_EQ(formatHttpHeaderLine(HttpHeader{}), "");
}

TEST(HttpHeader, RoundTrips) {
  const HttpHeader header{"Authorization", "Bearer a.b.c"};
  HttpHeader parsed;
  parseHttpHeaderLine(formatHttpHeaderLine(header), parsed);
  EXPECT_EQ(parsed.name, header.name);
  EXPECT_EQ(parsed.value, header.value);
}
