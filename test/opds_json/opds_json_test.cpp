// Host check for the OPDS 2.0 JSON parser (lib/OpdsParser/OpdsJsonParser).
//
// It exercises the shapes a real 2.0 catalog uses: navigation entries,
// publications with direct and indirect acquisition links, plain-EPUB
// preference, both author encodings, pagination and search links, group
// wrapping, entry truncation and malformed input. No device, no network.
//
// Run: bash test/opds_json/run_opds_json_test.sh

#include <OpdsJsonParser.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(const bool condition, const char* what) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

void checkString(const std::string& actual, const char* expected, const char* what) {
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s (expected \"%s\", got \"%s\")\n", what, expected, actual.c_str());
    ++failures;
  }
}

void feedAll(OpdsJsonParser& parser, const std::string& body, const size_t chunk) {
  for (size_t at = 0; at < body.size(); at += chunk) {
    const size_t n = (at + chunk < body.size()) ? chunk : body.size() - at;
    parser.write(reinterpret_cast<const uint8_t*>(body.data() + at), n);
  }
  parser.finish();
}

const char* kFeed = R"({
  "metadata": {"title": "Test Catalog"},
  "links": [
    {"rel": "self", "href": "/opds", "type": "application/opds+json"},
    {"rel": "search", "href": "/opds/search{?query}", "type": "application/opds+json"},
    {"rel": "next", "href": "/opds?page=2", "type": "application/opds+json"},
    {"rel": "previous", "href": "/opds?page=0", "type": "application/opds+json"}
  ],
  "navigation": [
    {"href": "/opds/new", "title": "New Releases", "rel": "subsection"},
    {"href": "/opds/authors", "title": "Authors"}
  ],
  "publications": [
    {
      "metadata": {
        "title": "Plain Book",
        "author": [{"name": "Ada Author"}, {"name": "Second Author"}],
        "description": "A book with a summary.",
        "identifier": "urn:uuid:plain"
      },
      "links": [
        {"rel": "http://opds-spec.org/acquisition", "href": "/dl/plain.mobi", "type": "application/x-mobipocket-ebook"},
        {"rel": "http://opds-spec.org/acquisition", "href": "/dl/plain.epub", "type": "application/epub+zip"}
      ]
    },
    {
      "metadata": {"title": "String Author", "author": "Grace Writer"},
      "links": [
        {"rel": "http://opds-spec.org/acquisition/open-access", "href": "/dl/string.epub", "type": "application/epub+zip"}
      ]
    },
    {
      "metadata": {"title": "Licensed Book"},
      "links": [
        {"rel": "http://opds-spec.org/acquisition",
         "href": "/licence/1",
         "type": "application/vnd.readium.lcp.license.v1.0+json",
         "properties": {"indirectAcquisition": [{"type": "application/epub+zip"}]}}
      ]
    },
    {
      "metadata": {"title": "No Download"},
      "links": [{"rel": "self", "href": "/pub/4", "type": "application/opds-publication+json"}]
    }
  ],
  "groups": [
    {
      "metadata": {"title": "Featured"},
      "navigation": [{"href": "/opds/featured", "title": "Featured Shelf"}],
      "publications": [
        {
          "metadata": {"title": "Grouped Book"},
          "links": [{"rel": "http://opds-spec.org/acquisition", "href": "/dl/grouped.epub", "type": "application/epub+zip"}]
        }
      ]
    }
  ]
})";

void testFullFeed() {
  OpdsJsonParser parser;
  feedAll(parser, kFeed, 7);  // small chunks: token splits across feeds
  check(!parser.error(), "feed parses without error");
  check(parser.recognized(), "feed is recognised as OPDS 2.0");
  check(!parser.truncated(), "feed fits the entry budget");

  const auto& entries = parser.getEntries();
  // 2 feed navigation rows + 2 downloadable publications + 1 grouped
  // navigation row + 1 grouped publication. The LCP licence wrapper and the
  // link-less publication carry no EPUB we can fetch, so they are dropped
  // exactly like an Atom entry without a href.
  check(entries.size() == 6, "navigation, publication and grouped rows are all collected");
  if (entries.size() != 6) return;

  // navigation[] first, in document order
  check(entries[0].type == OpdsEntryType::NAVIGATION, "row 0 is navigation");
  checkString(entries[0].title, "New Releases", "nav title");
  checkString(entries[0].href, "/opds/new", "nav href");
  checkString(entries[1].title, "Authors", "second nav title");

  // plain epub preferred over the .mobi sibling
  check(entries[2].type == OpdsEntryType::BOOK, "plain book is a book");
  checkString(entries[2].title, "Plain Book", "book title");
  checkString(entries[2].author, "Ada Author", "first author of an array wins");
  checkString(entries[2].summary, "A book with a summary.", "summary parsed");
  checkString(entries[2].href, "/dl/plain.epub", "plain epub preferred over derived format");
  check(!entries[2].indirectAcquisition, "direct acquisition is not marked indirect");

  checkString(entries[3].author, "Grace Writer", "string author form");
  checkString(entries[3].href, "/dl/string.epub", "acquisition/open-access rel accepted");

  // The LCP licence wrapper and the link-less publication never became rows:
  // no LCP client in this tree, and no href means nothing to download.
  for (const auto& entry : entries) {
    check(entry.title != "Licensed Book" && entry.title != "No Download", "unfetchable publications are skipped");
  }

  // groups[] wrappers contribute navigation and publication rows too
  checkString(entries[4].title, "Featured Shelf", "grouped navigation collected");
  checkString(entries[5].title, "Grouped Book", "grouped publication collected");
  checkString(entries[5].href, "/dl/grouped.epub", "grouped publication href");

  checkString(parser.getNextPageUrl(), "/opds?page=2", "next page link");
  checkString(parser.getPrevPageUrl(), "/opds?page=0", "previous page link");
  checkString(parser.getSearchTemplate(), "/opds/search{?query}", "search template");
}

void testTruncation() {
  std::string body = R"({"metadata":{"title":"Big"},"publications":[)";
  for (int i = 0; i < 80; ++i) {
    if (i) body += ',';
    body += R"({"metadata":{"title":"Book )" + std::to_string(i) +
            R"("},"links":[{"rel":"http://opds-spec.org/acquisition","href":"/b)" + std::to_string(i) +
            R"(.epub","type":"application/epub+zip"}]})";
  }
  body += "]}";

  OpdsJsonParser parser;
  feedAll(parser, body, 4096);
  check(!parser.error(), "oversized feed still parses");
  check(parser.getEntries().size() == 62, "entry budget is 62");
  check(parser.truncated(), "overflow is reported as truncated");
}

void testRejectsNonOpdsJson() {
  OpdsJsonParser parser;
  const std::string body = R"({"error":"not a catalog","items":[1,2,3]})";
  feedAll(parser, body, 3);
  check(!parser.error(), "valid JSON is not a syntax error");
  check(!parser.recognized(), "non-catalogue JSON is not accepted as OPDS");
}

void testMalformedJson() {
  OpdsJsonParser parser;
  const std::string body = R"({"metadata": {"title": "Broken", "publications": [})";
  feedAll(parser, body, 5);
  check(parser.error(), "malformed JSON reports an error");
}

void testEmptyFeed() {
  OpdsJsonParser parser;
  const std::string body = R"({"metadata":{"title":"Empty"},"navigation":[],"publications":[]})";
  feedAll(parser, body, 1000);
  check(!parser.error(), "empty catalog is valid");
  check(parser.getEntries().empty(), "empty catalog has no rows");
  check(parser.getNextPageUrl().empty(), "empty catalog has no next link");
}

void testSearchTermsTemplate() {
  OpdsJsonParser parser;
  const std::string body =
      R"({"links":[{"rel":"search","href":"/search?q={searchTerms}"}],"publications":[]})";
  feedAll(parser, body, 4);
  checkString(parser.getSearchTemplate(), "/search?q={searchTerms}", "1.x-style search template still accepted");
}
}  // namespace

int main() {
  testFullFeed();
  testTruncation();
  testRejectsNonOpdsJson();
  testMalformedJson();
  testEmptyFeed();
  testSearchTermsTemplate();

  if (failures != 0) {
    fprintf(stderr, "%d OPDS JSON check(s) failed\n", failures);
    return 1;
  }
  printf("OPDS 2.0 JSON parser: all checks passed\n");
  return 0;
}
