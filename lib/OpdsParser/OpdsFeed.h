#pragma once

#include <Print.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "OpdsEntry.h"
#include "OpdsJsonParser.h"
#include "OpdsParser.h"

/**
 * Streams one OPDS body into the parser that matches its format.
 *
 * OPDS 1.x is Atom XML; OPDS 2.0 is a Readium Web Publication Manifest in JSON.
 * The two share the row model (OpdsEntry) but nothing else, so the browser
 * activity talks to this dispatcher instead of either parser directly and stays
 * format-agnostic:
 *
 *   OpdsFeed feed;
 *   { OpdsParserStream stream{feed}; HttpDownloader::fetchUrl(url, stream, ...); }
 *   if (feed.error()) ...
 *   entries = feed.takeEntries();
 *
 * The format is sniffed from the first non-whitespace byte (`{` = JSON,
 * `<` = XML). Servers are inconsistent about Content-Type on catalogs, and
 * many answer `text/xml`/`application/octet-stream` for a 2.0 document, so the
 * payload itself is the authoritative signal. A UTF-8 BOM is tolerated.
 */
class OpdsFeed final : public Print {
 public:
  OpdsFeed();
  ~OpdsFeed() override = default;
  OpdsFeed(const OpdsFeed&) = delete;
  OpdsFeed& operator=(const OpdsFeed&) = delete;

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override;
  void flush() override;

  bool error() const;
  bool truncated() const;
  bool isJson() const { return json_ != nullptr; }
  bool isXml() const { return xml_ != nullptr; }

  const std::vector<OpdsEntry>& getEntries() const;
  std::vector<OpdsEntry> takeEntries();
  const std::string& getSearchTemplate() const;
  const std::string& getNextPageUrl() const;
  const std::string& getPrevPageUrl() const;

 private:
  void decide(uint8_t first);

  std::unique_ptr<OpdsParser> xml_;
  std::unique_ptr<OpdsJsonParser> json_;
  // Bytes received before the format was known (BOM / leading whitespace only;
  // capped so a whitespace flood cannot grow it).
  std::string pending_;
  bool malformed_ = false;
};
