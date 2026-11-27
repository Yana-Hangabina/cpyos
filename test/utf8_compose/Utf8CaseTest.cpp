#include <Utf8.h>
#include <gtest/gtest.h>

namespace {

// The mapping only has to make a small-caps run legible: every case that has an
// obvious 1:1 uppercase must map, and everything else must come back untouched
// (a wrong mapping would silently alter book text).
TEST(Utf8SmallCaps, MapsTheLatinGreekAndCyrillicCasePairs) {
  EXPECT_EQ(utf8UppercaseForSmallCaps('a'), 'A');
  EXPECT_EQ(utf8UppercaseForSmallCaps('z'), 'Z');
  EXPECT_EQ(utf8UppercaseForSmallCaps(0xE9), 0xC9);   // e acute
  EXPECT_EQ(utf8UppercaseForSmallCaps(0xFC), 0xDC);   // u diaeresis
  EXPECT_EQ(utf8UppercaseForSmallCaps(0xFF), 0x178);  // y diaeresis
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x131), 'I');   // dotless i
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x101), 0x100); // a macron
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x13A), 0x139); // l acute (inverted range)
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x3B1), 0x391); // alpha
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x3C2), 0x3A3); // final sigma
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x430), 0x410); // a (Cyrillic)
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x451), 0x401); // yo
}

TEST(Utf8SmallCaps, LeavesCaselessAndAlreadyUppercaseTextAlone) {
  EXPECT_EQ(utf8UppercaseForSmallCaps('A'), 'A');
  EXPECT_EQ(utf8UppercaseForSmallCaps('5'), '5');
  EXPECT_EQ(utf8UppercaseForSmallCaps(0xD7), 0xD7);   // multiplication sign
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x4E2D), 0x4E2D);  // CJK has no case
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x1F600), 0x1F600);  // emoji
  EXPECT_EQ(utf8UppercaseForSmallCaps(0x100), 0x100);  // already uppercase
}

}  // namespace
