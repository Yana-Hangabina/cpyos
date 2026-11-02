#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "OpdsEntry.h"

/**
 * Streaming parser for OPDS 2.0 catalogs (Readium Web Publication Manifest
 * shape: `metadata`, `links`, `navigation`, `publications`, optional `groups`).
 *
 * It produces the same `OpdsEntry` rows as the Atom/OPDS 1.x `OpdsParser`, so
 * the browser activity, its pagination rows and the download path are shared:
 *   - `navigation[]` items            -> OpdsEntryType::NAVIGATION
 *   - `publications[]` with an EPUB
 *     acquisition link                -> OpdsEntryType::BOOK
 *   - feed-level `links[]` with
 *     rel next/previous/search        -> nextPageUrl / prevPageUrl / searchTemplate
 *
 * Memory is bounded: at most MAX_ENTRIES rows, every string is capped, and the
 * JSON tokenizer (StreamingJsonParser) never buffers more than one 2 KiB token.
 *
 * No Arduino or Logging dependency so the parser is host-testable.
 */
class OpdsJsonParser final {
 public:
  OpdsJsonParser();
  ~OpdsJsonParser();
  OpdsJsonParser(const OpdsJsonParser&) = delete;
  OpdsJsonParser& operator=(const OpdsJsonParser&) = delete;

  /** Feed bytes; returns `len` (a streaming parser never asks for a retry). */
  size_t write(const uint8_t* data, size_t len);

  /**
   * Signal end of input. The tokenizer emits tokens at their delimiters, so a
   * body cut off mid-document (a dropped connection, a server error page in the
   * middle of a feed) would otherwise look like a short but valid catalog.
   * Marks an error when the root object was never closed.
   */
  bool finish();

  bool error() const;
  bool truncated() const { return truncated_; }
  /** True once a known OPDS 2.0 top-level key was seen (not just valid JSON). */
  bool recognized() const { return recognized_; }

  const std::vector<OpdsEntry>& getEntries() const { return entries_; }
  std::vector<OpdsEntry> takeEntries() { return std::move(entries_); }
  const std::string& getSearchTemplate() const { return searchTemplate_; }
  const std::string& getNextPageUrl() const { return nextPageUrl_; }
  const std::string& getPrevPageUrl() const { return prevPageUrl_; }

  void clear();

 private:
  // Structural context. The stack mirrors the JSON nesting so a string value
  // can be interpreted from the frame it belongs to (a `title` means different
  // things in navigation, publication metadata and group metadata).
  enum class Node : uint8_t {
    Root,
    Links,      // array of link objects (feed-level or publication-level)
    Link,       // one link object
    Properties, // link `properties`
    Indirect,   // properties.indirectAcquisition array
    IndirectItem,
    Nav,      // navigation array
    NavItem,  // one navigation object
    Pubs,     // publications array
    PubItem,  // one publication object
    Metadata, // publication / group / feed metadata object
    Images,   // publication images array
    Image,    // one image object
    Authors,  // metadata.author / contributor (array or object form)
    AuthorItem,
    Groups,    // groups array
    GroupItem, // one group object (holds its own navigation/publications)
    Other,     // anything we do not consume
  };
  struct Frame {
    Node node = Node::Other;
    bool array = false;
  };

  // One acquisition candidate, collected before the link object closes.
  struct PendingLink {
    std::string href;
    std::string rel;
    std::string type;
    bool indirectEpub = false;
    std::string indirectType;
  };

  static void onKey(void* ctx, const char* key, size_t len);
  static void onString(void* ctx, const char* value, size_t len);
  static void onNumber(void* ctx, const char* value, size_t len);
  static void onBool(void* ctx, bool value);
  static void onNull(void* ctx);
  static void onObjectStart(void* ctx);
  static void onObjectEnd(void* ctx);
  static void onArrayStart(void* ctx);
  static void onArrayEnd(void* ctx);

  Node keyed(Node parent, bool array) const;
  void push(Node node, bool array);
  void pop();
  const Frame* top() const { return depth_ > 0 ? &frames_[depth_ - 1] : nullptr; }
  bool keyIs(const char* name) const;

  void handleString(const char* value, size_t len);
  void finishLink();
  void finishNavigation();
  void finishPublication();
  void considerAcquisition();
  void assignAuthor(const char* value, size_t len);
  void startEntry();
  void commitEntry(OpdsEntry&& entry);

  StreamingJsonParser parser_;
  std::vector<OpdsEntry> entries_;
  Frame frames_[24];
  int depth_ = 0;
  char key_[32] = {0};
  bool keyValid_ = false;

  OpdsEntry current_;
  PendingLink link_;
  bool bookHrefIsPlainEpub_ = false;
  bool recognized_ = false;
  bool truncated_ = false;
  bool error_ = false;

  std::string searchTemplate_;
  std::string nextPageUrl_;
  std::string prevPageUrl_;
};
