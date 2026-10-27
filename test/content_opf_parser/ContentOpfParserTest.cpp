#include <gtest/gtest.h>

#include <string>

#include "ContentOpfParser.h"
#include "Epub/BookMetadataCache.h"

namespace {

void parse(ContentOpfParser& parser, const std::string& xml) {
  ASSERT_TRUE(parser.setup());
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
}

}  // namespace

TEST(ContentOpfParserMetadata, EntityCallbackDoesNotSplitOneAuthor) {
  const std::string xml =
      R"(<package xmlns:dc="urn:dc"><metadata><dc:creator>&#201;mile Zola</dc:creator></metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.author, "Émile Zola");
}

TEST(ContentOpfParserMetadata, ClampsOversizedMetadataTextInsteadOfGrowingUnbounded) {
  const std::string hugeTitle(64 * 1024, 'A');
  const std::string xml =
      "<package xmlns:dc=\"urn:dc\"><metadata><dc:title>" + hugeTitle + " tail</dc:title></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title.size(), 512u);
  EXPECT_EQ(parser.title[0], 'A');
}

TEST(ContentOpfParserMetadata, SeparatesCreatorElementsAndCollapsesXmlWhitespace) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>  The
   Left Hand   of Darkness  </dc:title>
    <dc:creator> Ursula   K. Le Guin </dc:creator>
    <dc:creator>
Octavia E. Butler
</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "The Left Hand of Darkness");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin, Octavia E. Butler");
}

TEST(ContentOpfParserMetadata, StopsBeforeManifestWithoutOpeningTemporaryStorage) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>A Wizard of Earthsea</dc:title>
    <dc:creator>Ursula K. Le Guin</dc:creator>
    <dc:language>en</dc:language>
  </metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  </package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.title, "A Wizard of Earthsea");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserMetadata, NeverEntersManifestWhenMetadataElementIsMissing) {
  const std::string xml =
      R"(<package><manifest><item id="chapter" href="chapter.xhtml"/></manifest><spine/></package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserCover, ResolvesEpub2CoverWithoutReadingCacheStorage) {
  const std::string xml = R"(<package><metadata><meta name="cover" content="cover-id"/></metadata>
    <manifest><item id="cover-id" href="cover.jpg" media-type="image/jpeg"/>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
    <spine><itemref idref="chapter"/></spine>
    <guide><reference type="cover" href="cover.xhtml"/></guide></package>)";
  const std::string cachePath = "/missing-cache";
  const std::string basePath = "OPS/";
  Storage = {};
  {
    ContentOpfParser parser(cachePath, basePath, xml.size(), nullptr);
    parse(parser, xml);
    EXPECT_EQ(parser.coverItemHref, "OPS/cover.jpg");
    EXPECT_EQ(parser.guideCoverPageHref, "OPS/cover.xhtml");
  }
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserCover, ResolvesEpub3CoverWithoutReadingCacheStorage) {
  const std::string xml = R"(<package><metadata/>
    <manifest><item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/></manifest>
    <spine/></package>)";
  const std::string cachePath = "/missing-cache";
  const std::string basePath = "OPS/";
  Storage = {};
  {
    ContentOpfParser parser(cachePath, basePath, xml.size(), nullptr);
    parse(parser, xml);
    EXPECT_EQ(parser.coverItemHref, "OPS/cover.png");
  }
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserCover, ReadingParserStillOpensManifestCache) {
  const std::string xml = R"(<package><metadata/>
    <manifest><item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/></manifest>
    <spine/></package>)";
  const std::string cachePath = "/reading-cache";
  const std::string basePath = "OPS/";
  BookMetadataCache cache;
  Storage = {};
  {
    ContentOpfParser parser(cachePath, basePath, xml.size(), &cache);
    parse(parser, xml);
    EXPECT_EQ(parser.coverItemHref, "OPS/cover.png");
  }
  EXPECT_EQ(Storage.writeOpens, 1);
  EXPECT_EQ(Storage.readOpens, 1);
}

// A5: font files declared in the manifest are collected (absolute, normalised
// paths like cssFiles), by media-type or, failing that, by extension.
TEST(ContentOpfParserFonts, CollectsManifestFontItems) {
  const std::string xml = R"(<package><metadata/>
    <manifest>
      <item id="f1" href="fonts/body.otf" media-type="application/vnd.ms-opentype"/>
      <item id="f2" href="fonts/deco.ttf" media-type="font/ttf"/>
      <item id="f3" href="mystery.bin" media-type="application/octet-stream"/>
      <item id="f4" href="style.css" media-type="text/css"/>
      <item id="f5" href="loose.ttf"/>
    </manifest><spine/></package>)";
  Storage = {};
  {
    ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr);
    parse(parser, xml);
    ASSERT_EQ(parser.fontFiles.size(), 3u);
    EXPECT_EQ(parser.fontFiles[0], "OPS/fonts/body.otf");
    EXPECT_EQ(parser.fontFiles[1], "OPS/fonts/deco.ttf");
    // Extension fallback when the manifest omits the media-type.
    EXPECT_EQ(parser.fontFiles[2], "OPS/loose.ttf");
    // Font collection must not disturb the CSS list.
    ASSERT_EQ(parser.cssFiles.size(), 1u);
    EXPECT_EQ(parser.cssFiles[0], "OPS/style.css");
  }
}

TEST(ContentOpfParserFonts, IgnoresNonFontItems) {
  const std::string xml = R"(<package><metadata/>
    <manifest>
      <item id="c" href="chapter.xhtml" media-type="application/xhtml+xml"/>
      <item id="i" href="img.png" media-type="image/png"/>
    </manifest><spine/></package>)";
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_TRUE(parser.fontFiles.empty());
}
