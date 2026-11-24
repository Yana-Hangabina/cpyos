#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "FirstLineIndent.h"
#include "WordStore.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  struct LineBreakState {
    int cost;
    size_t breakIndex;
  };

  // Word text lives in wordStore (chunked bump arena, NUL-terminated entries);
  // words holds 8-byte handles into it. This replaces the former
  // std::deque<std::string>: per-word string objects, their SSO spills, and
  // every hyphenation/NFC temporary were the layout path's dominant
  // small-allocation churn, and any failed implicit allocation abort()s under
  // -fno-exceptions. Handles stay in a std::deque for the #2814 reason: no
  // large contiguous reallocation at CJK token counts (deque grows in fixed
  // ~512 B nodes). On arena OOM the word is dropped and hadDroppedWords()
  // latches so the section build can fail readably instead of aborting.
  // rubyTexts stays a deque of strings: ruby is rare and per-block small.
  // The per-token parallel arrays below stay vectors: 1 byte / 1 bit each,
  // they never approach the contiguous-block ceiling.
  WordStore wordStore;
  std::deque<WordStore::StoredWord> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Boundary flags use all four combinations:
  //   continues=false, noSpace=false: ordinary breakable word gap
  //   continues=false, noSpace=true:  breakable zero-width, stretchable CJK/Korean gap
  //   continues=true,  noSpace=false: unbreakable attachment
  //   continues=true,  noSpace=true:  breakable zero-width, non-stretching attachment
  std::vector<bool> wordContinues;
  std::vector<bool> wordNoSpaceBefore;
  // Focus Reading emphasis: bytes [0, wordFocusBoundary) render bold, the rest at wordStyles.
  // 0 = none. An annotation rather than a token split, so the hyphenator and line breaker still
  // see whole words; TextBlock stores emphasis the same way, so extractLine passes it through.
  std::vector<uint8_t> wordFocusBoundary;
  // Internal-link identity through tokenization, hyphenation and BiDi reorder.
  // Zero means plain text; non-zero indexes linkTargets. Kept at one byte per
  // token and discarded after layout, never added to the page-cache TextBlock.
  std::vector<uint8_t> wordLinkIds;
  std::vector<std::string> linkTargets;
  // Per-word face of an inline `font-size` run (layout milestone M5b). A slot of 0
  // means the block's face. The vector stays EMPTY for a paragraph that never changes
  // size, so plain text pays neither the byte per word nor a slot table; once a run
  // appears it is kept in lockstep with `words` (backfilled with zeros).
  std::vector<uint8_t> wordFontSlots;
  int32_t wordFontIds[3] = {0, 0, 0};
  uint8_t wordFontIdCount = 0;
  bool wordFontSlotOverflowLogged = false;
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks do not carry any of this metadata.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  std::deque<std::string> rubyTexts;
  BlockStyle blockStyle;
  uint8_t extraParagraphSpacing;  // 0=off, 1..5=0.5x/0.75x/1x/1.25x/1.5x line height
  uint8_t firstLineIndent;
  int8_t firstLineIndentPx;  // fine adjustment (-12..+12 px) added to the resolved indent
  // True when the reader's indent mode overrides the book (Indent/NoIndent), in
  // which case a hand-typed paragraph indent (U+3000 pairs are the norm in
  // Chinese web novels) is stripped so the reader's setting is the only one.
  bool stripLeadingIndent;
  bool collectTouchLinks;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphIndentSpaces;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool firstLinePending;
  bool paragraphStartPending = true;  // strips the leading hand-typed indent once
  bool isNaturalAlign;
  bool hasRtlWord;
  bool droppedWords = false;
  // Drop-cap reservation (layout milestone M6). A block whose ::first-letter rule asks
  // for one lays its first dropCapLines lines out to the right of the enlarged glyph.
  // Both stay 0 for every other block, which keeps that layout byte for byte.
  uint8_t dropCapLines = 0;
  int16_t dropCapWidth = 0;
  bool dropCapActive = false;
  // Lines this block still has after the one `layoutAndExtractLines` is currently
  // handing to its callback; -1 outside that window. Layout-only flow feedback for
  // orphan/widow control, never serialized.
  int linesAfterCurrent = -1;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  std::string_view wordAt(const size_t i) const { return wordStore.view(words[i]); }
  bool storeWord(std::string_view text, WordStore::StoredWord& out);
  uint8_t slotForFontId(int32_t fontId);
  void pushWordFontSlot(uint8_t slot);
  void insertWordFontSlot(size_t index, uint8_t slot);
  // Renderer face to measure word `index` with: its slot's id, or the line's face.
  int fontIdAt(size_t index, int fontId) const;
  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  bool computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, std::vector<uint16_t>& wordWidths,
                         std::vector<bool>& continuesVec, std::vector<bool>& noSpaceBeforeVec,
                         std::unique_ptr<LineBreakState[]>& workspace, size_t& lineBreakCount);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  // Line ends of the block's first dropCapLines lines, measured against their reduced
  // width. Empty when no drop cap is reserved or the block is right-to-left. The cap is
  // at most three lines, so the result is a fixed array: this runs once per extracted
  // line while a cap is active, and a std::vector there would churn the heap on every
  // line for no benefit.
  struct DropCapBreaks {
    static constexpr uint8_t kMax = 4;
    size_t ends[kMax] = {};
    uint8_t count = 0;
    void push(const size_t end) {
      if (count < kMax) ends[count++] = end;
    }
    const size_t* begin() const { return ends; }
    const size_t* end() const { return ends + count; }
  };
  DropCapBreaks computeDropCapLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                         const std::vector<uint16_t>& wordWidths,
                                         const std::vector<bool>& continuesVec,
                                         const std::vector<bool>& noSpaceBeforeVec) const;
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  bool extractLine(size_t lineIndex, size_t lineBreak, size_t lastBreakAt, size_t lineCount, int pageWidth,
                   const std::vector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                   const std::vector<bool>& noSpaceBeforeVec,
                   const std::function<bool(std::unique_ptr<TextBlock>, uint32_t, bool)>& processLine,
                   const GfxRenderer& renderer, int fontId, bool paragraphFinal);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const uint8_t extraParagraphSpacing, const uint8_t firstLineIndent,
                      const bool hyphenationEnabled = false, const bool focusReadingEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const bool collectTouchLinks = false,
                      const uint8_t paragraphIndentSpaces = 3, const int8_t firstLineIndentPx = 0)
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        firstLineIndent(firstLineIndent),
        firstLineIndentPx(firstLineIndentPx),
        stripLeadingIndent(firstLineIndent != FirstLineIndent::Auto),
        paragraphIndentSpaces(paragraphIndentSpaces),
        collectTouchLinks(collectTouchLinks),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        firstLinePending(true),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  // A block reused across a heading boundary (e.g. a <br> inside an h2) must
  // pick up the new hyphenation state.
  void setHyphenationEnabled(const bool enabled) { hyphenationEnabled = enabled; }
  bool isHyphenationEnabled() const { return hyphenationEnabled; }

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0, uint8_t linkId = 0, int32_t fontId = 0);
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  bool hadDroppedWords() const { return droppedWords; }
  int linesAfterCurrentLine() const { return linesAfterCurrent; }
  // Ask for a drop-cap reservation before layout: `lines` text lines start `width` px
  // in from the block's left edge. Ignored (and reported by the getter) in RTL text,
  // where the enlarged glyph has no column to sit in.
  void setDropCapReservation(const int width, const uint8_t lines) {
    dropCapWidth = static_cast<int16_t>(width);
    dropCapLines = lines;
  }
  bool dropCapReservationActive() const { return dropCapActive; }
  bool layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<bool(std::unique_ptr<TextBlock>, uint32_t, bool)>& processLine,
                             bool includeLastLine = true, int8_t characterSpacing = 0,
                             uint8_t wordSpacingPercent = 100);
};
