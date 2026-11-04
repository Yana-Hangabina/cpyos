#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace webbrowser {

enum class HtmlTokenType : uint8_t { End, Text, StartTag, EndTag };

struct HtmlAttribute {
  std::string_view name;
  std::string_view value;
};

struct HtmlToken {
  HtmlTokenType type = HtmlTokenType::End;
  std::string_view name;  // lowercased tag name
  std::string_view text;  // entity-decoded text (Text tokens)
  bool selfClosing = false;
  uint8_t attributeCount = 0;
  HtmlAttribute attributes[8]{};

  std::string_view attribute(const std::string_view wanted) const {
    for (uint8_t i = 0; i < attributeCount; ++i) {
      if (attributes[i].name == wanted) return attributes[i].value;
    }
    return {};
  }
};

// A tolerant HTML tokenizer, not a parser.
//
// expat (the EPUB path) rejects everything a real page does: unquoted
// attributes, void elements without '/', stray '&', '<' inside script. This
// scanner accepts all of that, never validates nesting, and never allocates
// beyond one token's scratch text. The extractor above it decides what the
// markup means.
//
// Handles: comments, doctypes, processing instructions, raw-text elements
// (script/style/title/textarea/xmp), quoted and unquoted attributes, numeric
// and common named entities.
class HtmlScanner {
 public:
  explicit HtmlScanner(const std::string_view html) : html_(html) {}

  // Returns false at end of input. A returned Text token is only valid until
  // the next call.
  bool next(HtmlToken& out);

 private:
  bool emitText(HtmlToken& out);
  bool parseStartTag(HtmlToken& out);
  bool parseEndTag(HtmlToken& out);
  void skipDeclaration();
  bool inRawText() const { return !rawTag_.empty(); }
  bool emitRawText(HtmlToken& out);

  std::string_view html_;
  size_t pos_ = 0;
  std::string nameBuf_;
  std::string attrNames_[8];
  std::string attrValues_[8];
  std::string textBuf_;
  std::string rawTag_;
};

}  // namespace webbrowser
