#include "OpdsJsonParser.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {
constexpr size_t MAX_ENTRIES = 62;  // same ceiling as the Atom parser's storage
constexpr size_t MAX_TITLE_CHARS = 160;
constexpr size_t MAX_AUTHOR_CHARS = 120;
constexpr size_t MAX_SUMMARY_CHARS = 240;
constexpr size_t MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 768;
constexpr size_t MAX_REL_CHARS = 96;
constexpr size_t MAX_TYPE_CHARS = 128;
constexpr size_t MAX_SEARCH_TEMPLATE_CHARS = 768;
constexpr size_t MAX_PAGE_URL_CHARS = 768;

void assignBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  const size_t n = len < maxLen ? len : maxLen;
  target.assign(value, n);
}

bool containsNoCase(const std::string& haystack, const char* needle) {
  if (needle[0] == '\0') return true;
  const size_t n = strlen(needle);
  if (haystack.size() < n) return false;
  for (size_t i = 0; i + n <= haystack.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < n; ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

// A link relation is either the bare token ("next", "search") or the full
// OPDS acquisition URI ("http://opds-spec.org/acquisition/open-access").
bool isAcquisitionRel(const std::string& rel) {
  static constexpr char kPrefix[] = "http://opds-spec.org/acquisition";
  return rel.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0;
}

bool isTemplatedSearchUrl(const std::string& href) {
  // OPDS 2.0 templates follow RFC 6570: {searchTerms} or {?query} / {&query}.
  return href.find('{') != std::string::npos;
}
}  // namespace

OpdsJsonParser::OpdsJsonParser()
    : parser_(JsonCallbacks{this, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd,
                            onArrayStart, onArrayEnd, nullptr}) {}

OpdsJsonParser::~OpdsJsonParser() = default;

size_t OpdsJsonParser::write(const uint8_t* data, size_t len) {
  if (error_ || len == 0) return len;
  parser_.feed(reinterpret_cast<const char*>(data), len);
  if (parser_.hasError()) {
    error_ = true;
  }
  return len;
}

bool OpdsJsonParser::finish() {
  if (depth_ != 0) error_ = true;
  return !error();
}

bool OpdsJsonParser::error() const { return error_ || parser_.hasError(); }

void OpdsJsonParser::clear() {
  entries_.clear();
  searchTemplate_.clear();
  nextPageUrl_.clear();
  prevPageUrl_.clear();
  current_ = OpdsEntry{};
  link_ = PendingLink{};
  depth_ = 0;
  key_[0] = '\0';
  keyValid_ = false;
  recognized_ = false;
  truncated_ = false;
  error_ = false;
  bookHrefIsPlainEpub_ = false;
  parser_.reset();
}

bool OpdsJsonParser::keyIs(const char* name) const {
  return keyValid_ && strcmp(key_, name) == 0;
}

void OpdsJsonParser::push(const Node node, const bool array) {
  constexpr int kMaxFrames = static_cast<int>(sizeof(frames_) / sizeof(frames_[0]));
  if (depth_ >= kMaxFrames) {
    // Deeper than any real OPDS document: stop feeding values into it, and let
    // the frame at the top decide. StreamingJsonParser itself errors past its
    // own nesting limit, which surfaces as a parse error.
    return;
  }
  frames_[depth_++] = Frame{node, array};
}

void OpdsJsonParser::pop() {
  if (depth_ > 0) --depth_;
}

OpdsJsonParser::Node OpdsJsonParser::keyed(const Node parent, const bool array) const {
  if (array) {
    switch (parent) {
      case Node::Root:
      case Node::GroupItem:
        if (keyIs("links")) return Node::Links;
        if (keyIs("navigation")) return Node::Nav;
        if (keyIs("publications")) return Node::Pubs;
        if (keyIs("groups") && parent == Node::Root) return Node::Groups;
        break;
      case Node::PubItem:
        if (keyIs("links")) return Node::Links;
        if (keyIs("images")) return Node::Images;
        break;
      case Node::Properties:
        if (keyIs("indirectAcquisition")) return Node::Indirect;
        break;
      case Node::Metadata:
        if (keyIs("author") || keyIs("contributor")) return Node::Authors;
        break;
      default:
        break;
    }
    return Node::Other;
  }

  switch (parent) {
    case Node::Root:
      if (keyIs("metadata")) return Node::Metadata;
      break;
    case Node::Links:
      return Node::Link;
    case Node::Link:
      if (keyIs("properties")) return Node::Properties;
      break;
    case Node::Indirect:
      return Node::IndirectItem;
    case Node::Nav:
      return Node::NavItem;
    case Node::Pubs:
      return Node::PubItem;
    case Node::PubItem:
      if (keyIs("metadata")) return Node::Metadata;
      break;
    case Node::Images:
      return Node::Image;
    case Node::Metadata:
      if (keyIs("author") || keyIs("contributor")) return Node::Authors;
      break;
    case Node::Authors:
      return Node::AuthorItem;
    case Node::Groups:
      return Node::GroupItem;
    case Node::GroupItem:
      if (keyIs("metadata")) return Node::Metadata;
      break;
    default:
      break;
  }
  return Node::Other;
}

void OpdsJsonParser::startEntry() {
  current_ = OpdsEntry{};
  link_ = PendingLink{};
  bookHrefIsPlainEpub_ = false;
}

void OpdsJsonParser::commitEntry(OpdsEntry&& entry) {
  if (entries_.size() >= MAX_ENTRIES) {
    truncated_ = true;
    return;
  }
  entries_.push_back(std::move(entry));
}

void OpdsJsonParser::onKey(void* ctx, const char* key, const size_t len) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  const size_t n = std::min(len, sizeof(self->key_) - 1);
  memcpy(self->key_, key, n);
  self->key_[n] = '\0';
  self->keyValid_ = true;

  // A top-level key is what makes this an OPDS 2.0 document rather than any
  // other JSON body (an error page, a token endpoint, ...).
  if (self->depth_ == 1) {
    if (self->keyIs("metadata") || self->keyIs("links") || self->keyIs("navigation") ||
        self->keyIs("publications") || self->keyIs("groups")) {
      self->recognized_ = true;
    }
  }
}

void OpdsJsonParser::onObjectStart(void* ctx) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  if (self->depth_ == 0) {
    self->push(Node::Root, false);
    return;
  }
  const Frame* parent = self->top();
  if (!parent) return;
  const Node node = self->keyed(parent->node, false);
  if (node == Node::NavItem || node == Node::PubItem) self->startEntry();
  self->push(node, false);
}

void OpdsJsonParser::onObjectEnd(void* ctx) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  const Frame* top = self->top();
  if (!top) return;
  const Node node = top->node;
  switch (node) {
    case Node::Link:
      self->pop();
      self->finishLink();
      break;
    case Node::NavItem:
      self->pop();
      self->finishNavigation();
      break;
    case Node::PubItem:
      self->pop();
      self->finishPublication();
      break;
    default:
      self->pop();
      break;
  }
  self->keyValid_ = false;
}

void OpdsJsonParser::onArrayStart(void* ctx) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  const Frame* parent = self->top();
  self->push(parent ? self->keyed(parent->node, true) : Node::Other, true);
}

void OpdsJsonParser::onArrayEnd(void* ctx) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  self->pop();
  self->keyValid_ = false;
}

void OpdsJsonParser::onNumber(void*, const char*, size_t) {}
void OpdsJsonParser::onBool(void*, bool) {}
void OpdsJsonParser::onNull(void*) {}

void OpdsJsonParser::handleString(const char* value, const size_t len) {
  if (!keyValid_) return;
  const Frame* top = this->top();
  if (!top) return;

  switch (top->node) {
    case Node::Link:
      if (keyIs("href")) {
        assignBounded(link_.href, value, len, MAX_HREF_CHARS);
      } else if (keyIs("rel")) {
        assignBounded(link_.rel, value, len, MAX_REL_CHARS);
      } else if (keyIs("type")) {
        assignBounded(link_.type, value, len, MAX_TYPE_CHARS);
      }
      break;

    case Node::IndirectItem:
      if (keyIs("type")) {
        assignBounded(link_.indirectType, value, len, MAX_TYPE_CHARS);
        if (containsNoCase(link_.indirectType, "epub")) link_.indirectEpub = true;
      }
      break;

    case Node::NavItem:
      if (keyIs("title")) {
        assignBounded(current_.title, value, len, MAX_TITLE_CHARS);
      } else if (keyIs("href")) {
        assignBounded(current_.href, value, len, MAX_HREF_CHARS);
      }
      break;

    case Node::Metadata: {
      // Only a publication's metadata describes a row; the feed's and a
      // group's metadata carry catalog titles we do not display.
      const bool publicationMetadata = depth_ >= 2 && frames_[depth_ - 2].node == Node::PubItem;
      if (!publicationMetadata) break;
      if (keyIs("title")) {
        assignBounded(current_.title, value, len, MAX_TITLE_CHARS);
      } else if (keyIs("description")) {
        assignBounded(current_.summary, value, len, MAX_SUMMARY_CHARS);
      } else if (keyIs("author") || keyIs("contributor")) {
        assignAuthor(value, len);
      } else if (keyIs("identifier")) {
        assignBounded(current_.id, value, len, MAX_ID_CHARS);
      }
      break;
    }

    case Node::Authors:
      // `"author": "Name"` and `"author": ["Name"]` both land here.
      assignAuthor(value, len);
      break;

    case Node::AuthorItem:
      if (keyIs("name")) assignAuthor(value, len);
      break;

    default:
      break;
  }
}

void OpdsJsonParser::onString(void* ctx, const char* value, const size_t len) {
  static_cast<OpdsJsonParser*>(ctx)->handleString(value, len);
}

void OpdsJsonParser::assignAuthor(const char* value, const size_t len) {
  if (!current_.author.empty()) return;  // first author wins, like the Atom parser
  assignBounded(current_.author, value, len, MAX_AUTHOR_CHARS);
}

void OpdsJsonParser::finishLink() {
  // After the Link object was popped, the frame below is its `links` array and
  // the one below that is its owner (feed root / group / publication).
  const Node owner = (depth_ >= 2) ? frames_[depth_ - 2].node : Node::Other;
  if (owner == Node::Root || owner == Node::GroupItem) {
    const std::string& rel = link_.rel;
    if (rel == "next") {
      assignBounded(nextPageUrl_, link_.href.c_str(), link_.href.size(), MAX_PAGE_URL_CHARS);
    } else if (rel == "previous" || rel == "prev") {
      assignBounded(prevPageUrl_, link_.href.c_str(), link_.href.size(), MAX_PAGE_URL_CHARS);
    } else if (rel == "search" && isTemplatedSearchUrl(link_.href)) {
      assignBounded(searchTemplate_, link_.href.c_str(), link_.href.size(), MAX_SEARCH_TEMPLATE_CHARS);
    }
  } else if (owner == Node::PubItem) {
    if (isAcquisitionRel(link_.rel)) considerAcquisition();
  }
  link_ = PendingLink{};
}

void OpdsJsonParser::considerAcquisition() {
  if (link_.href.empty()) return;

  const bool typeIsEpub = containsNoCase(link_.type, "epub");
  bool accept = typeIsEpub;
  if (!accept && link_.indirectEpub) {
    // An indirect acquisition points at a wrapper document whose payload is an
    // EPUB. Follow it only when the wrapper itself is a catalog/entry document
    // this client can fetch. A licence wrapper (LCP) needs an LCP client the
    // tree does not ship, so it is skipped instead of downloading a non-book.
    accept = link_.type.empty() || containsNoCase(link_.type, "atom") || containsNoCase(link_.type, "opds");
  }
  if (!accept) return;

  const bool isPlainEpub = link_.href.find(".epub") != std::string::npos ||
                           link_.href.find("/epub/") != std::string::npos;
  // Prefer a plain EPUB over derived/multi-format links, same rule as 1.x.
  // Among equals the first link wins, again matching the Atom parser.
  if (current_.type == OpdsEntryType::BOOK) {
    if (bookHrefIsPlainEpub_) return;
    if (!isPlainEpub) return;
  }

  current_.type = OpdsEntryType::BOOK;
  assignBounded(current_.href, link_.href.c_str(), link_.href.size(), MAX_HREF_CHARS);
  current_.indirectAcquisition = link_.indirectEpub;
  bookHrefIsPlainEpub_ = isPlainEpub;
}

void OpdsJsonParser::finishNavigation() {
  if (current_.title.empty() || current_.href.empty()) return;
  current_.type = OpdsEntryType::NAVIGATION;
  commitEntry(std::move(current_));
  current_ = OpdsEntry{};
}

void OpdsJsonParser::finishPublication() {
  if (current_.title.empty() || current_.href.empty()) {
    current_ = OpdsEntry{};
    return;
  }
  if (current_.type != OpdsEntryType::BOOK) current_.type = OpdsEntryType::BOOK;
  commitEntry(std::move(current_));
  current_ = OpdsEntry{};
}
