#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <array>
#include <climits>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/PageBreakPolicy.h"
#include "Epub/PageLink.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/BlockFontProvider.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class PageBorderBox;
class PageDropCap;
class PageElement;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // Depth of an open h1-h6, INT_MAX outside one. Blocks started inside it are
  // not hyphenated: a title split across lines mid-word reads badly.
  int headingDepth = INT_MAX;
  bool blockHyphenationEnabled() const { return hyphenationEnabled && headingDepth == INT_MAX; }
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  // Bottom of the last text line actually pushed onto the current page (the
  // layout cursor without any trailing paragraph spacing appended by
  // endParagraph). applyVerticalBottomAlign() uses it so a page's last line can
  // be pushed flush to the content bottom without counting that gap as consumed.
  int16_t currentPageContentBottom = 0;
  // Height of the most recent text line pushed onto the current page; the
  // safety cap for how much a single inter-line gap may grow.
  int16_t currentLineHeight = 0;
  // Which page element each of the current page's links belongs to, so a shifted
  // line moves its links by the same delta. Bounded by PageLinkList::MAX_SIZE,
  // so tracking costs 64 bytes and no allocation.
  std::array<uint16_t, PageLinkList::MAX_SIZE> currentPageLinkLines{};
  // A page carrying table rows is never redistributed: rows reset the layout
  // cursor per cell, so the plain line count/bottom math does not describe them
  // (upstream 9a7e6b35).
  bool currentPageHasGridTable = false;
  // Page-break carry state (layout milestone M3). A page hands whole trailing text
  // lines to the next one so a paragraph never ends with an orphan or opens with a
  // widow, and a heading travels with the block it introduces. See PageBreakPolicy.h
  // for the decision itself; nothing here is serialized.
  ParsedText* layoutParagraph = nullptr;  // block being laid out, if any
  uint8_t paragraphLinesOnPage = 0;       // of that block, already on this page
  uint8_t keepWithNextLines = 0;          // heading lines waiting for the next block
  // Offsets of the page's trailing lines, and how many footnotes each already had
  // when it was pushed, so carried lines restore the next page's start position and
  // take their footnotes with them. Bounded by the carry limit: 30 bytes, no heap.
  std::array<uint32_t, page_break_policy::kMaxCarriedLines> recentLineOffsets{};
  std::array<uint8_t, page_break_policy::kMaxCarriedLines> recentFootnoteStarts{};
  size_t recentLineCount = 0;
  bool pendingPageBreak = false;  // a block just closed with page-break-after
  // Box decoration of the block being laid out (layout milestone M4b). The element is
  // appended just before the block's first line on a page -- after the page-break
  // decision, so it never needs an existing element index shifted -- and its height is
  // filled in when the block, or the part of it that fits, reaches its last line. At
  // most one box per block per page; the box only exists in horizontal writing.
  struct BorderBoxState {
    PageBorderBox* element = nullptr;
    int16_t top = 0;
    uint8_t bottomEdge = 0;
  } borderBox;
  // The block's box was cut by a page break, so its continuation omits the top edge.
  // Cleared when a new block starts laying out.
  bool borderBoxContinues = false;
  // Drop cap of the block being laid out (layout milestone M6). The block's first
  // visible character becomes a scaled page element, and the block's first lines are
  // laid out to its right. Everything here stays zero for a block whose ::first-letter
  // rule does not ask for one, so plain paragraphs lay out exactly as before.
  struct DropCapState {
    uint32_t codepoint = 0;
    char utf8[5] = {};
    uint8_t utf8Len = 0;
    uint16_t scale256 = 0;  // 0 = this block has no cap
    uint16_t width = 0;     // scaled ink box of the glyph
    uint16_t height = 0;
    int32_t faceId = 0;  // 0 = the section's reader face
    uint8_t lines = 0;   // text lines the glyph spans
    bool letterPending = true;  // the block's first character is still to be taken
    bool emitPending = false;   // the element still has to be placed on a page
    PageDropCap* element = nullptr;
    PageElement* line = nullptr;  // the PageLine the cap sits next to
  } dropCap;
  uint16_t pendingDropCapScale = 0;  // request of the element being opened
  bool dropCapLogged = false;        // one line per section, not per paragraph
  bool verticalText = false;  // boxes have no column mapping, so vertical layout emits none
  int fontId;
  float lineCompression;
  uint8_t extraParagraphSpacing;  // 0=off, 1..5=0.5x/0.75x/1x/1.25x/1.5x line height
  uint8_t firstLineIndent;
  int8_t firstLineIndentPx = 0;  // fine adjustment added to the resolved indent
  uint8_t paragraphIndentSpaces = 3;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphAlignment;
  bool verticalBottomAlign;  // redistribute non-final text pages so the last line reaches the content bottom
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  const CssParser* cssParser;
  // Optional: resolves a block's CSS font scale to a renderer face. Null keeps every
  // block on `fontId` (reader size), which is what host tests and the dictionary
  // preview use.
  BlockFontProvider* blockFonts = nullptr;
  bool embeddedStyle;
  bool collectTouchLinks;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool setsParagraphDirection = false;
    bool hasTextAlign = false;
    CssTextAlign textAlign = CssTextAlign::Left;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
    bool hasSmallCaps = false, smallCaps = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;

  // Whole-block sizing heuristic (upstream 0305aaa6): a sizing span that opens a block
  // and holds all of its text sizes the whole block, e.g.
  // <p><span class="big">Chapter One</span></p>. Body text following the span cancels
  // it, because then only the span's own words should have been large. Parser-only:
  // the resolved scale is folded into BlockStyle::fontScale when the block is laid out.
  struct InlineSizeState {
    float scale = 1.0f;
    int depth = -1;  // outermost sizing span
    bool valid = false;
    bool open = false;
  } inlineSize;

  // Per-word faces (layout milestone M5b). An inline `font-size` that resolves to another
  // face records it here for the words added while that element is open; the entries nest,
  // so the innermost open run wins. Empty for a book without inline sizes.
  struct InlineFontRun {
    int depth = 0;
    int32_t fontId = 0;
    float scale = 1.0f;  // this run's size, the parent of a nested one
  };
  std::vector<InlineFontRun> inlineFontStack;
  int32_t effectiveWordFontId = 0;
  void beginInlineFontRun(const char* name, const CssStyle& cssStyle);

  // Open-element identities for descendant/child selector matching (upstream 0305aaa6).
  // Indexed by parser depth: entry [depth] is written when that element opens, so the valid
  // ancestors of an element are [0, depth). Nothing needs popping. 16 entries x 28 B = 448 B,
  // inside this heap-allocated parser and never growing. Elements nested deeper than 16 are
  // resolved without ancestors (contextual rules simply do not match), which bounds the stack.
  static constexpr size_t MAX_CSS_ANCESTORS = 16;
  std::array<CssAncestor, MAX_CSS_ANCESTORS> cssAncestors{};

  // `white-space` inheritance. Frames are pushed only when the effective value changes, so the
  // stack tracks boundaries rather than every element; 16 x {int, bool} is 128 B.
  struct WhiteSpaceFrame {
    int depth = 0;
    bool preserve = false;
  };
  static constexpr size_t MAX_WHITESPACE_FRAMES = 16;
  std::array<WhiteSpaceFrame, MAX_WHITESPACE_FRAMES> whitespaceStack{};
  uint8_t whitespaceStackSize = 0;
  bool effectivePreserveWhitespace = false;
  // Resolved ::first-letter style of the innermost open block. Only filled when the stylesheet
  // actually declares first-letter rules; later layout milestones (drop caps) consume it.
  CssStyle firstLetterStyle;

  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveTextAlignDefined = false;
  CssTextAlign effectiveTextAlign = CssTextAlign::Left;
  bool effectiveSup = false;
  bool effectiveSmallCaps = false;
  bool effectiveSub = false;
  static constexpr size_t MAX_GRID_TABLE_COLUMNS = 4;
  static constexpr size_t MAX_GRID_TABLE_CELL_WORDS = 32;
  static constexpr size_t MAX_GRID_TABLE_CELL_BYTES = 512;
  int tableDepth = 0;
  bool insideTableCell = false;
  bool tableRowStacked = false;
  bool tableRowRtl = false;
  uint16_t tableRowsSpannedRemaining = 0;
  size_t tableCellTextBytes = 0;
  std::vector<std::unique_ptr<ParsedText>> tableRowCells;
  std::array<std::vector<std::unique_ptr<TextBlock>>, MAX_GRID_TABLE_COLUMNS> tableCellLines;
  std::vector<uint32_t> tableLineVisibleOffsets;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // Tracks the innermost open <ul>/<ol> so <li> knows whether to number itself,
  // bullet itself, or (list-style-type: none) emit no marker at all. Pushed on
  // <ul>/<ol> open, popped on close, so nested lists restart their own counter
  // without disturbing the parent list's.
  struct ListContext {
    bool ordered = false;    // true for <ol>, false for <ul>
    bool styleNone = false;  // true when list-style-type: none is set on this list
    int counter = 0;         // incremented before each direct <li>; used as its number when ordered
    int depth = 0;           // parser depth at open time; matches the depth seen in endElement
                             // for the same tag, so a hidden nested list's close can't pop
                             // an outer list's context
  };
  std::vector<ListContext> listStack;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on. A deque grows in
  // small chunks, so a long chapter never needs one large free block (a vector doubling past 512
  // anchors needs ~28 KB contiguous, and a failed allocation aborts).
  int completedPageCount = 0;
  std::deque<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;  // deferred until after previous text block is flushed
  bool txtChapterBoundaries = false;
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  // The <p> currently being read, and the index latched onto the text block that
  // will be laid out later. endElement() clears activeIndex at </p>, but the block
  // is only extracted on the next block start (or at finish), so the lines must
  // use the latched value -- using active there stamped every line 0xFFFF.
  uint16_t activeParagraphIndex = 0xFFFF;
  uint16_t currentBlockParagraphIndex = 0xFFFF;
  // Canonical reading-position counter: zero-based Unicode codepoints in visible
  // <body> text. Token offsets flow through line breaking so every completed page
  // records the first source character it renders.
  uint32_t visibleTextOffset = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  bool allocationFailed_ = false;
  bool ioFailed_ = false;
  bool parseActive_ = false;
  void stopParsing();
  void failAllocation(const char* stage);
  bool checkMemory();
  bool insideBody = false;
  bool htmlEnded_ = false;
  bool syntheticCharacterData = false;
  uint16_t nonVisibleTextDepth = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  uint8_t currentFootnoteLinkId = 0;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;
  // Latched when a ParsedText could not be created (OOM). Together with
  // ParsedText::hadDroppedWords() this turns layout OOM into ParseStatus::Error
  // so the section build fails readably instead of emitting pages with holes.
  bool layoutOom = false;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  void softFlushTextBlock();
  // `white-space: pre` / <pre> text: a run is emitted as one attached word token so its spaces
  // keep their exact width, and a newline ends the current line.
  void flushPreservedRun();
  void breakPreservedLine();
  void fallbackTableRowToStacked();
  void closeTableCell();
  void finishTableRow();
  void addTableRowSeparator();
  void setCurrentPageVisibleOffset(uint32_t offset);
  bool allocatePage();
  void makePages();
  // Honour page-break-after from the block that just closed and page-break-before on
  // the block about to place content. A break never emits a blank page.
  void applyPendingPageBreak();
  [[nodiscard]] page_break_policy::Carry linesToCarry() const;
  [[nodiscard]] bool canCarry(size_t lines) const;
  // Complete the page, moving its trailing `carry.lines` text lines and their link
  // areas to the top of the next page with their spacing intact.
  void breakPageCarryingLines(page_break_policy::Carry carry, uint32_t visibleOffset);
  // Fold this element's font-size (or the browser default for <h1>..<h6>) into the
  // block style, clamped to the reader's supported 0.5x..3x range.
  void applyBlockFontScale(BlockStyle& blockStyle, const CssStyle& cssStyle, const char* tagName) const;
  // Resolve the scale the block being laid out should use, and record the chosen face
  // in its BlockStyle. Returns the font id to lay the block out with (always valid:
  // falls back to the section font).
  int prepareBlockFont(ParsedText& text, float scale);
  // Append the block's box before its first line on this page; returns false only on OOM.
  bool openBorderBox(const BlockStyle& blockStyle);
  // Fill in the box height at the block's last line on this page; a `cutBottom` close
  // is a page break and drops the bottom edge. A box left without lines is dropped.
  void closeBorderBox(int16_t bottom, bool cutBottom);
  void dropBorderBox();
  // Read an element's ::first-letter font-size as a drop-cap request: at least twice the
  // block's own size and clamped to the 2x..3x the primitive draws well.
  void requestDropCap(const CssStyle& firstLetterStyle, float blockScale);
  // Hand the pending request to the block that is about to start.
  void applyPendingDropCap();
  // Take the block's first visible character out of the pending word for the cap.
  bool consumeDropCapLetter();
  // Scaled ink box of the cap's glyph in one face, at the request's scale.
  bool measureDropCapGlyph(int face, uint32_t codepoint, const char* utf8, size_t utf8Len, int& width,
                           int& height) const;
  // Resolve the cap's metrics against the face the block is laid out with and reserve
  // its column. False leaves the block plain.
  bool layoutDropCap(ParsedText& text, int layoutFontId, int lineHeight);
  void resetDropCap();
  // Face for a block scale, or 0 when the block keeps the section font.
  int fontIdForScale(float scale) const;
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushTableTextStyleEntry(const CssStyle& cssStyle);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
      const float lineCompression, const uint8_t extraParagraphSpacing, const uint8_t firstLineIndent,
      const int8_t firstLineIndentPx, const uint8_t paragraphAlignment, const bool verticalBottomAlign,
      const uint16_t viewportWidth, const uint16_t viewportHeight,
      const bool hyphenationEnabled, const bool focusReadingEnabled,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
      const bool collectTouchLinks = false, BlockFontProvider* blockFonts = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        firstLineIndent(firstLineIndent),
        firstLineIndentPx(firstLineIndentPx),
        paragraphAlignment(paragraphAlignment),
        verticalBottomAlign(verticalBottomAlign),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(embeddedStyle ? cssParser : nullptr),
        blockFonts(blockFonts),
        embeddedStyle(embeddedStyle),
        collectTouchLinks(collectTouchLinks),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();
  void setTextSpacing(const int8_t character, const uint8_t wordPercent) {
    characterSpacing = character;
    wordSpacingPercent = wordPercent;
  }
  void setTxtChapterBoundaries(bool enabled) { txtChapterBoundaries = enabled; }
  void setParagraphIndentSpaces(const uint8_t spaces) { paragraphIndentSpaces = spaces; }
  void setVerticalText(const bool enabled) { verticalText = enabled; }

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   More: keep going / yield; Done: finishParse(); Error / OutOfMemory: abortParse().
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error, OutOfMemory };
  bool allocationFailed() const { return allocationFailed_; }
  bool ioFailed() const { return ioFailed_; }
  bool hasFailed() const { return allocationFailed_ || ioFailed_; }
  void failIo() {
    ioFailed_ = true;
    stopParsing();
  }
  bool beginParse();
  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  bool addLineToPage(std::unique_ptr<TextBlock> line, uint32_t visibleOffset);
  const std::deque<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Emit the current page through completePageFn. `doBottomAlign` redistributes
  // a text-only page so the first line stays at the top and the last one reaches
  // viewportHeight, shifting the page's links in step. The chapter's final page
  // and pages closed at a chapter boundary pass false so they stay top-aligned.
  void completeCurrentPage(bool doBottomAlign);
  void applyVerticalBottomAlign(Page* page);

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
