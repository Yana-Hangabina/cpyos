#include "DictionaryDefinitionActivity.h"
#include "components/StatusCards.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "DictionaryWordSelectActivity.h"
#include "ReaderUtils.h"
#include "components/Rect.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"
#include "util/PlainTextTokens.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

// Slack kept above an allocation that grows with the definition text.
constexpr size_t HEAP_HEADROOM = 8 * 1024;

constexpr unsigned long MESSAGE_DURATION_MS = 1500;

}  // namespace

// cppcheck-suppress constParameterReference // the lookup state holds the dictionary by non-const reference
bool DictionaryDefinitionActivity::enableLookup(Dictionary& dictionary, std::string&& query) {
  lookup = makeUniqueNoThrow<LookupState>(dictionary);
  if (!lookup) LOG_ERR("DICT", "OOM: lookup history (%u bytes)", static_cast<unsigned>(sizeof(LookupState)));
  if (lookup) lookup->query = std::move(query);
  return lookup != nullptr;
}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  initialLayoutFailed = !prepareDefinition(content);
  if (initialLayoutFailed) {
    content.lines.clear();
    showMessage(StrId::STR_DICT_LOW_MEMORY);
  }
  requestUpdate();
}

bool DictionaryDefinitionActivity::prepareDefinition(Definition& next, const bool forcePlain) {
  // Normalize StarDict multi-type separators so the wrap loop and the C-string
  // font APIs below both see the whole definition.
  std::replace(next.text.begin(), next.text.end(), '\0', '\n');
  if (htmlDefinition && !forcePlain && next.text.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages(next)) return true;
  if (htmlDefinition) {
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
    // htmlToPlainText allocates a second text-sized buffer; refuse while the
    // previous definition's release has not paid for it yet.
    if (ESP.getFreeHeap() < next.text.size() + 1 + HEAP_HEADROOM ||
        ESP.getMaxAllocHeap() < next.text.size() + 1) {
      LOG_ERR("DICT", "Low heap for plain definition (%u bytes)", static_cast<unsigned>(next.text.size() + 1));
      return false;
    }
    next.text = htmlToPlainText(next.text);
  }
  return wrapText(next);
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;
  const int topArea = contentY + metrics.topPadding + metrics.headerHeight;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;
  return {contentX + SIDE_PADDING, topArea, contentWidth - 2 * SIDE_PADDING,
          renderer.getScreenHeight() - topArea - bottomArea};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `text` on success (the page arenas own
// the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages(Definition& next) {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, next.text, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), next.pages)) {
    return false;
  }
  std::string{}.swap(next.text);
  next.totalPages = static_cast<int>(next.pages.size());
  next.currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Greedy word-wrap of `text` into byte spans. '\n' breaks lines (blank lines
// survive as paragraph spacing; NULs from multi-type StarDict entries were
// normalized to newlines in prepareDefinition); '\r' is dropped by treating it
// as a space at a token edge.
bool DictionaryDefinitionActivity::wrapText(Definition& next) {
  auto& lines = next.lines;
  const auto& definition = next.text;
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  next.linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  next.totalPages = std::max(1, (static_cast<int>(lines.size()) + next.linesPerPage - 1) / next.linesPerPage);
  next.currentPage = 0;
  return true;
}

// Plain definitions keep only line spans; the selection screen needs word
// boxes, so pack one page's visible lines into a Page for the child activity.
// Bounded to a single page and released as soon as the child returns.
bool DictionaryDefinitionActivity::buildPlainSelectionPage() {
  selectionPage = makeUniqueNoThrow<Page>();
  if (!selectionPage) return false;

  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId);
  const int firstLine = content.currentPage * content.linesPerPage;
  const int lastLine = std::min(firstLine + content.linesPerPage, static_cast<int>(content.lines.size()));
  if (lastLine <= firstLine) return false;

  renderer.ensureSdCardFontReady(fontId, content.text.c_str(), 0x01 /* REGULAR */);
  selectionPage->elements.reserve(static_cast<size_t>(lastLine - firstLine));

  std::vector<PlainTextToken> tokens;
  tokens.reserve(32);
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<EpdFontFamily::Style> styles;
  char buf[MAX_LINE_BYTES + 1];

  for (int row = firstLine; row < lastLine; row++) {
    const Line& line = content.lines[row];
    if (line.len == 0) continue;
    const size_t length = std::min(static_cast<size_t>(line.len), MAX_LINE_BYTES);
    const char* base = content.text.c_str() + line.start;
    tokenizePlainTextLine(base, length, tokens);
    if (tokens.empty()) continue;

    words.clear();
    xpos.clear();
    styles.clear();
    words.reserve(tokens.size());
    xpos.reserve(tokens.size());
    styles.reserve(tokens.size());
    for (const auto& token : tokens) {
      memcpy(buf, base + token.start, token.len);
      buf[token.len] = '\0';
      words.emplace_back(buf);
      // Measure the full prefix so repeated interior whitespace in the source
      // span still lands the box where drawText paints the word.
      xpos.push_back(static_cast<int16_t>(measureSpan(fontId, base, token.start)));
      styles.push_back(EpdFontFamily::REGULAR);
    }
    auto block = makeUniqueNoThrow<TextBlock>(words, xpos, styles, std::vector<uint8_t>{},
                                              std::vector<uint16_t>{});
    if (!block || !block->valid()) return false;
    auto element = makeUniqueNoThrow<PageLine>(std::move(block), 0, static_cast<int16_t>((row - firstLine) * lineHeight));
    if (!element) return false;
    selectionPage->elements.push_back(std::move(element));
  }
  return true;
}

void DictionaryDefinitionActivity::startSelection() {
  if (!lookup) return;
  const BodyArea body = bodyArea();
  if (!content.pages.empty()) {
    if (content.currentPage < 0 || content.currentPage >= static_cast<int>(content.pages.size())) return;
    if (!startActivityForResultWith<DictionaryWordSelectActivity>(
            [this](const ActivityResult& result) {
              RenderLock lock;
              if (const auto* selected = std::get_if<DictionarySelectionResult>(&result.data)) {
                if (lookup->depth >= lookup->history.size()) {
                  showMessage(StrId::STR_DICT_HISTORY_FULL);
                  return;
                }
                lookup->pendingWord = selected->word;
              }
            },
            *content.pages[content.currentPage], body.x, body.y)) {
      showMessage(StrId::STR_DICT_LOW_MEMORY);
    }
    return;
  }
  {
    if (!buildPlainSelectionPage()) {
      LOG_ERR("DICT", "Cannot allocate plain definition selection");
      selectionPage.reset();
      showMessage(StrId::STR_DICT_LOW_MEMORY);
      return;
    }
  }
  // The child borrows selectionPage; it is reset by the result handler below.
  if (!startActivityForResultWith<DictionaryWordSelectActivity>(
          [this](const ActivityResult& result) {
            RenderLock lock;
            selectionPage.reset();
            if (const auto* selected = std::get_if<DictionarySelectionResult>(&result.data)) {
              if (lookup->depth >= lookup->history.size()) {
                showMessage(StrId::STR_DICT_HISTORY_FULL);
                return;
              }
              lookup->pendingWord = selected->word;
            }
          },
          *selectionPage, body.x, body.y)) {
    selectionPage.reset();
    showMessage(StrId::STR_DICT_LOW_MEMORY);
  }
}

void DictionaryDefinitionActivity::showMessage(const StrId nextMessage) {
  message = nextMessage;
  messageTime = millis();
  messageVisible = true;
  requestUpdate();
}

bool DictionaryDefinitionActivity::restoreCurrentDefinition(const bool styled) {
  const int page = content.currentPage;
  auto result = Dictionary::LookupResult::Found;
  if (!lookup->dictionary.lookup(lookup->query.c_str(), content.text, content.headword, &result) ||
      !prepareDefinition(content, !styled) || styled != !content.pages.empty() || page >= content.totalPages) {
    LOG_ERR("DICT", "Cannot recover current definition after failed lookup");
    finish();
    return false;
  }
  content.currentPage = page;
  return true;
}

void DictionaryDefinitionActivity::navigateDefinition(const bool back) {
  {
    RenderLock lock;
    showMessage(StrId::STR_DICT_LOOKING_UP);
  }
  requestUpdateAndWait();

  RenderLock lock;
  messageVisible = false;
  const bool styled = !content.pages.empty();
  // Keep only navigation metadata while inflating and laying out another entry.
  content.releasePayload();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();

  Definition next;
  auto result = Dictionary::LookupResult::Found;
  const char* word = back ? lookup->history[lookup->depth - 1].query.c_str() : lookup->pendingWord.c_str();
  const bool found = lookup->dictionary.lookup(word, next.text, next.headword, &result);
  const auto recover = [this, back, styled, &next](const StrId error) {
    lookup->pendingWord.clear();
    next.releasePayload();
    std::string{}.swap(next.headword);
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
    if (back) {
      LOG_ERR("DICT", "Cannot restore previous definition; leaving lookup");
      finish();
    } else if (restoreCurrentDefinition(styled)) {
      showMessage(error);
    }
  };
  if (!found) {
    switch (result) {
      case Dictionary::LookupResult::Found:
      case Dictionary::LookupResult::ReadError:
        recover(StrId::STR_DICT_READ_FAILED);
        break;
      case Dictionary::LookupResult::NotFound:
        recover(StrId::STR_DICT_NOT_FOUND);
        break;
      case Dictionary::LookupResult::LowMemory:
        recover(StrId::STR_DICT_LOW_MEMORY);
        break;
      case Dictionary::LookupResult::Decompress:
        recover(StrId::STR_DICT_DECOMPRESS_ERROR);
        break;
    }
    return;
  }
  const bool forcePlain = back && !lookup->history[lookup->depth - 1].styled;
  if (!prepareDefinition(next, forcePlain)) {
    recover(StrId::STR_DICT_LOW_MEMORY);
    return;
  }
  if (back) {
    auto& previous = lookup->history[lookup->depth - 1];
    if (previous.styled != !next.pages.empty() || previous.page >= next.totalPages) {
      LOG_ERR("DICT", "Cannot restore definition layout");
      recover(StrId::STR_DICT_RESTORE_FAILED);
      return;
    }
    lookup->query = std::move(previous.query);
    next.currentPage = previous.page;
    --lookup->depth;
  } else {
    auto& previous = lookup->history[lookup->depth++];
    previous.query = std::move(lookup->query);
    lookup->query = std::move(lookup->pendingWord);
    previous.page = content.currentPage;
    previous.styled = styled;
  }
  lookup->pendingWord.clear();
  content = std::move(next);
  requestUpdate();
}

void DictionaryDefinitionActivity::loop() {
  if (initialLayoutFailed) {
    if (millis() - messageTime >= MESSAGE_DURATION_MS) finish();
    return;
  }
  if (lookup && !lookup->pendingWord.empty()) {
    navigateDefinition(false);
    return;
  }
  if (messageVisible && millis() - messageTime >= MESSAGE_DURATION_MS) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      messageVisible = false;
      requestUpdate();
    }
  }

  int sx = 0;
  int sy = 0;
  const bool longPress = lookup && mappedInput.wasScreenLongPress(sx, sy);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Long Back leaves the whole lookup chain; a short one steps back one
    // definition, and only the first definition exits to book word selection.
    if (lookup && lookup->depth > 0 && mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
      navigateDefinition(true);
    } else {
      finish();
    }
    return;
  }
  if (longPress || (lookup && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    startSelection();
    return;
  }

  const int totalPages = content.totalPages;
  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (content.currentPage > 0) {
        content.currentPage--;
        requestUpdate();
      }
    } else if (content.currentPage + 1 < totalPages) {
      content.currentPage++;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (content.currentPage + 1 < content.totalPages) {
      content.currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (content.currentPage > 0) {
      content.currentPage--;
      requestUpdate();
    }
  });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY,
                                           const int currentPage) const {
  const auto& pages = content.pages;
  const auto& lines = content.lines;
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * content.linesPerPage;
  const int lastLine = std::min(firstLine + content.linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, content.text.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf);
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  // Header: the theme's header band (title + page counter). On touch boards it
  // paints a back button and registers its tap target, which becomes the
  // visible way back through the definition stack.
  char counter[16] = {};
  if (content.totalPages > 1) {
    snprintf(counter, sizeof(counter), "%d/%d", content.currentPage + 1, content.totalPages);
  }
  GUI.drawHeader(renderer, Rect{contentX, contentY + metrics.topPadding, contentWidth, metrics.headerHeight},
                 content.headword.c_str(), counter[0] ? counter : nullptr);

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const BodyArea body = bodyArea();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, body.x, body.y, content.currentPage);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, body.x, body.y, content.currentPage);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), lookup ? tr(STR_LOOKUP) : "", (content.currentPage > 0 ? "<" : ""),
                            (content.currentPage + 1 < content.totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (messageVisible) {
    status_card::showMessage(renderer, I18N.get(message), nullptr);
    return;
  }
  renderer.displayBuffer();
}
