#pragma once

#include <Print.h>
#include <Stream.h>

#include "OpdsParser.h"

// Print adapter so HttpDownloader::fetchUrl(url, Stream&) can feed a parser.
// Takes any Print (OpdsParser for Atom, OpdsFeed for either format).
class OpdsParserStream : public Stream {
 public:
  explicit OpdsParserStream(Print& parser);

  // That functions are not implemented for that stream
  int available() override;
  int peek() override;
  int read() override;

  virtual size_t write(uint8_t c) override;
  virtual size_t write(const uint8_t* buffer, size_t size) override;

  ~OpdsParserStream() override;

 private:
  Print& parser;
};
