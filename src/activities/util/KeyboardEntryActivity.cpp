#include "KeyboardEntryActivity.h"

#include <BidiUtils.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "KeyboardLayoutSet.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiHighDpiProfile.h"
#include "components/themes/upstream/UpstreamStyleTheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr fui::ActionId ACTION_KEY = 1;
constexpr int KEYBOARD_MIN_GAP = 6;
constexpr int KEYBOARD_PANEL_PADDING = 4;
constexpr int BUTTON_KEY_HEIGHT = 42;
constexpr int BUTTON_KEYBOARD_HINT_GAP = 4;

// Candidate strip under the input field. Only the last 128 bytes of committed
// text ever reach the engine (it looks at most 24 back), matching kiikoread's cap.
constexpr size_t PRECEDING_MAX = 128;
constexpr int STRIP_CELL_PADDING = 16;
// Was 44: at 297 ppi that is ~3.8 mm, the smallest touch target in the UI
// (keyboard keys are 88 px). Chinese candidates are tapped far more often than
// any single key, so give them a real target.
constexpr int STRIP_MIN_CELL_WIDTH = 56;

int stripHeight(const bool highDpi) { return highDpi ? 84 : 48; }

int keyboardGap(const ThemeMetrics& metrics, const bool compact = false) {
  return compact ? KEYBOARD_MIN_GAP : std::max(metrics.keyboardKeySpacing, KEYBOARD_MIN_GAP);
}

int keyboardKeysHeight(const ThemeMetrics& metrics, const int rows, const bool hasTouch) {
  const int keyHeight = hasTouch ? metrics.keyboardKeyHeight : BUTTON_KEY_HEIGHT;
  return rows * keyHeight + (rows > 1 ? (rows - 1) * keyboardGap(metrics) : 0) + KEYBOARD_PANEL_PADDING * 2;
}

// ---------------------------------------------------------------------------
// URL layers. The SDK builtin layouts have no URL variant (":", "/", ".", the
// snippet panel), so these are app-defined tables over the same public
// KeyboardLayout structs. URLs are ASCII, so the letter rows are EN-arranged
// regardless of UI language.
// ---------------------------------------------------------------------------

constexpr uint8_t URL_KEY_WIDTH = 2;
constexpr uint8_t URL_WIDE_CONTROL_WIDTH = 3;

#define UK(label, output, value) \
  fui::KeyboardKey { label, output, fui::KeyKind::Normal, fui::StateNormal, value, URL_KEY_WIDTH, true, nullptr }
#define UKA(label, output, value, alt) \
  fui::KeyboardKey { label, output, fui::KeyKind::Normal, fui::StateNormal, value, URL_KEY_WIDTH, true, alt }
#define UKW(label, output, value, units) \
  fui::KeyboardKey { label, output, fui::KeyKind::Normal, fui::StateNormal, value, units *URL_KEY_WIDTH, true, nullptr }
#define UKS(label, kind, value, units) \
  fui::KeyboardKey { label, nullptr, kind, fui::StateNormal, value, units *URL_KEY_WIDTH, true, nullptr }
#define UK15(label, kind, value) \
  fui::KeyboardKey { label, nullptr, kind, fui::StateNormal, value, URL_WIDE_CONTROL_WIDTH, true, nullptr }

constexpr int16_t URL_PANEL_VALUE = -3;  // mirrors KeyboardEntryActivity::URL_PANEL_KEY

const fui::KeyboardKey URL_NUM_ROW[] = {UKA("1", "1", '1', "!"), UKA("2", "2", '2', "@"), UKA("3", "3", '3', "#"),
                                        UKA("4", "4", '4', "$"), UKA("5", "5", '5', "%"), UKA("6", "6", '6', "^"),
                                        UKA("7", "7", '7', "&"), UKA("8", "8", '8', "*"), UKA("9", "9", '9', "("),
                                        UKA("0", "0", '0', ")")};

const fui::KeyboardKey URL_ROW1[] = {UK("q", "q", 'q'), UK("w", "w", 'w'), UK("e", "e", 'e'), UK("r", "r", 'r'),
                                     UK("t", "t", 't'), UK("y", "y", 'y'), UK("u", "u", 'u'), UK("i", "i", 'i'),
                                     UK("o", "o", 'o'), UK("p", "p", 'p')};
const fui::KeyboardKey URL_ROW2[] = {UK("a", "a", 'a'), UK("s", "s", 's'), UK("d", "d", 'd'),
                                     UK("f", "f", 'f'), UK("g", "g", 'g'), UK("h", "h", 'h'),
                                     UK("j", "j", 'j'), UK("k", "k", 'k'), UK("l", "l", 'l')};
const fui::KeyboardKey URL_ROW3[] = {UK15(nullptr, fui::KeyKind::Shift, fui::QWERTY_KEY_SHIFT),
                                     UK("z", "z", 'z'),
                                     UK("x", "x", 'x'),
                                     UK("c", "c", 'c'),
                                     UK("v", "v", 'v'),
                                     UK("b", "b", 'b'),
                                     UK("n", "n", 'n'),
                                     UK("m", "m", 'm'),
                                     UK15("Del", fui::KeyKind::Delete, fui::QWERTY_KEY_BACKSPACE)};
// URLs have no spaces, so the URL bottom row spends the space slot on ":",
// "/", "." and the snippet-panel toggle instead (the legacy keyboard did the
// same with its "URL" key).
const fui::KeyboardKey URL_BOTTOM[] = {UKS("?123", fui::KeyKind::Mode, fui::QWERTY_KEY_MODE, 2),
                                       UK(":", ":", ':'),
                                       UK("/", "/", '/'),
                                       UK(".", ".", '.'),
                                       UKW("URL", nullptr, URL_PANEL_VALUE, 3),
                                       UKS("OK", fui::KeyKind::Ok, fui::QWERTY_KEY_ENTER, 2)};

const fui::KeyboardKey URL_SHIFT_ROW1[] = {UK("Q", "Q", 'Q'), UK("W", "W", 'W'), UK("E", "E", 'E'), UK("R", "R", 'R'),
                                           UK("T", "T", 'T'), UK("Y", "Y", 'Y'), UK("U", "U", 'U'), UK("I", "I", 'I'),
                                           UK("O", "O", 'O'), UK("P", "P", 'P')};
const fui::KeyboardKey URL_SHIFT_ROW2[] = {UK("A", "A", 'A'), UK("S", "S", 'S'), UK("D", "D", 'D'),
                                           UK("F", "F", 'F'), UK("G", "G", 'G'), UK("H", "H", 'H'),
                                           UK("J", "J", 'J'), UK("K", "K", 'K'), UK("L", "L", 'L')};
const fui::KeyboardKey URL_SHIFT_ROW3[] = {UK15(nullptr, fui::KeyKind::Shift, fui::QWERTY_KEY_SHIFT),
                                           UK("Z", "Z", 'Z'),
                                           UK("X", "X", 'X'),
                                           UK("C", "C", 'C'),
                                           UK("V", "V", 'V'),
                                           UK("B", "B", 'B'),
                                           UK("N", "N", 'N'),
                                           UK("M", "M", 'M'),
                                           UK15("Del", fui::KeyKind::Delete, fui::QWERTY_KEY_BACKSPACE)};

// Snippet keys: multi-character outputs, stable ids above the localized-key
// range so they never collide with layout key ids.
const fui::KeyboardKey URL_SNIP_ROW1[] = {UK("https://", "https://", 2001), UK("www.", "www.", 2002),
                                          UK(".com", ".com", 2003)};
const fui::KeyboardKey URL_SNIP_ROW2[] = {UK("http://", "http://", 2004), UK("192.168.", "192.168.", 2005),
                                          UK(".org", ".org", 2006)};
const fui::KeyboardKey URL_SNIP_ROW3[] = {UK("/opds", "/opds", 2007), UK(":8080", ":8080", 2008),
                                          UK(".net", ".net", 2009)};
const fui::KeyboardKey URL_SNIP_BOTTOM[] = {UKS("abc", fui::KeyKind::Mode, fui::QWERTY_KEY_MODE, 2),
                                            UKW("URL", nullptr, URL_PANEL_VALUE, 3),
                                            UK15("Del", fui::KeyKind::Delete, fui::QWERTY_KEY_BACKSPACE),
                                            UKS("OK", fui::KeyKind::Ok, fui::QWERTY_KEY_ENTER, 2)};

#undef UK
#undef UKA
#undef UKW
#undef UKS
#undef UK15

const fui::KeyboardRow URL_ROWS[] = {
    {URL_NUM_ROW, 10, 0}, {URL_ROW1, 10, 0}, {URL_ROW2, 9, 1}, {URL_ROW3, 9, 0}, {URL_BOTTOM, 6, 0}};
const fui::KeyboardRow URL_SHIFT_ROWS[] = {
    {URL_NUM_ROW, 10, 0}, {URL_SHIFT_ROW1, 10, 0}, {URL_SHIFT_ROW2, 9, 1}, {URL_SHIFT_ROW3, 9, 0}, {URL_BOTTOM, 6, 0}};
const fui::KeyboardRow URL_SNIP_ROWS[] = {
    {URL_SNIP_ROW1, 3, 0}, {URL_SNIP_ROW2, 3, 0}, {URL_SNIP_ROW3, 3, 0}, {URL_SNIP_BOTTOM, 4, 0}};

const fui::KeyboardLayout URL_LAYOUT{URL_ROWS, 5};
const fui::KeyboardLayout URL_SHIFT_LAYOUT{URL_SHIFT_ROWS, 5};
const fui::KeyboardLayout URL_SNIPPET_LAYOUT{URL_SNIP_ROWS, 4};

}  // namespace

void KeyboardEntryActivity::setLiveResults(LiveResultsFn fn, void* ctx) {
  liveResultsFn_ = fn;
  liveResultsCtx_ = ctx;
}

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  cursorPos = text.length();
  // URL layers are EN-arranged app tables; everything else opens on the UI
  // language's layout, or on an enabled one if the user switched that off.
  layoutId = inputType == InputType::Url ? fui::KeyboardLayoutId::QwertyEn : keyboard_layouts::startingLayout();
  // The key only earns its slot in the bottom row with somewhere to go.
  const uint16_t enabledLayouts = keyboard_layouts::enabled();
  showLangKey = (enabledLayouts & (enabledLayouts - 1)) != 0;
  shifted = false;
  symbols = false;
  urlPanel = false;
  cursorMode = false;
  togglePos = false;
  passwordVisible = false;
  selRow = 0;
  selCol = 0;
  delPressCount = 0;
  hintVisible = false;
  hintShowTime = 0;
  rightHeld = false;
  rightLongHandled = false;
  savedCursorPos = 0;
  rightStartCursorPos = 0;
  touchRouter.reset();
  touchRouter.holdMs = TOUCH_LONG_PRESS_MS;
  touchRouter.overrideHoldMs = TOUCH_DEL_LONG_PRESS_MS;
  interactionsReady = false;
  // Chinese entry needs a free-form text field (accounts, codes and numbers
  // opt out via InputType::Ascii), the Pinyin database, PSRAM for it, touch to
  // pick candidates, and room above the keyboard for the strip — without any
  // of those the globe never appears and the keyboard is exactly the old one.
  // The load happens here, not on the first letter, so no keystroke pays half
  // a second for it; on a card without the resource (and on no-PSRAM builds)
  // this is a cheap no-op.
  chineseMode = false;
  imeActive = false;
  composition.clear();
  candidateSkip = 0;
  chineseAvailable = inputType == InputType::Text && mappedInput.hasTouch() &&
                     pinyin::PinyinDb::shared().ensureLoaded();
  if (chineseAvailable) {
    StripLayout probe;
    layoutStrip(probe);
    chineseAvailable = !probe.strip.empty();
  }
  requestUpdate();
}

void KeyboardEntryActivity::onExit() {
  Activity::onExit();
  // Release the PSRAM session with the screen that owns it.
  ime.end();
  imeActive = false;
  chineseMode = false;
}

const fui::KeyboardLayout& KeyboardEntryActivity::baseLayout() const {
  if (symbols) return fui::builtinKeyboardLayout(layoutId, shifted, true, false, false);
  if (inputType == InputType::Url) {
    if (urlPanel) return URL_SNIPPET_LAYOUT;
    return shifted ? URL_SHIFT_LAYOUT : URL_LAYOUT;
  }
  return fui::builtinKeyboardLayout(layoutId, shifted, false, true, showLangKey);
}

// The globe sits in the bottom row's leftmost slot, before the mode key. Its
// two units come out of the mode key and the space bar so the row still adds
// up to the same width and OK keeps its size; a row too tight for that gets a
// one-unit globe instead. The globe's value is app-owned (-5), so nothing
// collides with the SDK's own control ids (-1..-4).
int KeyboardEntryActivity::globeLogicalIndex() const {
  const fui::KeyboardLayout& layout = currentLayout();
  if (layout.rowCount == 0) return -1;
  int index = 0;
  for (int row = 0; row + 1 < layout.rowCount; row++) index += layout.rows[row].count;
  return index;
}

const fui::KeyboardLayout& KeyboardEntryActivity::currentLayout() const {
  const fui::KeyboardLayout& base = baseLayout();
  if (!chineseAvailable || base.rows == nullptr || base.rowCount == 0) return base;
  if (chineseBaseCache == &base) return chineseLayout_;
  const size_t bottom = base.rowCount - 1;
  const fui::KeyboardRow& source = base.rows[bottom];
  if (source.keys == nullptr || source.insetUnits != 0 || source.count + 1 > std::size(chineseKeyCache_)) return base;

  for (size_t i = 0; i <= bottom; i++) chineseRowCache_[i] = base.rows[i];
  size_t modeIndex = SIZE_MAX;
  size_t spaceIndex = SIZE_MAX;
  for (size_t i = 0; i < source.count; i++) {
    chineseKeyCache_[i + 1] = source.keys[i];
    if (source.keys[i].kind == fui::KeyKind::Mode) modeIndex = i + 1;
    if (source.keys[i].kind == fui::KeyKind::Space) spaceIndex = i + 1;
  }
  uint8_t globeUnits = 2;
  if (modeIndex != SIZE_MAX && chineseKeyCache_[modeIndex].widthUnits >= 2) {
    chineseKeyCache_[modeIndex].widthUnits -= 1;
  } else {
    globeUnits = 1;
  }
  if (spaceIndex != SIZE_MAX && chineseKeyCache_[spaceIndex].widthUnits >= 3) {
    chineseKeyCache_[spaceIndex].widthUnits -= 1;
  } else if (globeUnits == 2) {
    globeUnits = 1;
  }
  chineseKeyCache_[0] = fui::KeyboardKey{nullptr, nullptr, fui::KeyKind::Lang, fui::StateNormal, CHINESE_KEY,
                                         globeUnits, true, nullptr};
  chineseRowCache_[bottom] =
      fui::KeyboardRow{chineseKeyCache_, static_cast<uint8_t>(source.count + 1), 0, source.independentKeyWidth};
  chineseLayout_ = fui::KeyboardLayout{chineseRowCache_, base.rowCount};
  chineseBaseCache = &base;
  return chineseLayout_;
}

const fui::KeyboardKey* KeyboardEntryActivity::selectedKey() const {
  const fui::KeyboardLayout& layout = currentLayout();
  if (selRow < 0 || selRow >= layout.rowCount) return nullptr;
  const fui::KeyboardRow& row = layout.rows[selRow];
  if (selCol < 0 || selCol >= row.count) return nullptr;
  return &row.keys[selCol];
}

int KeyboardEntryActivity::selectedLogicalIndex() const {
  const fui::KeyboardLayout& layout = currentLayout();
  int index = 0;
  for (int r = 0; r < selRow && r < layout.rowCount; r++) {
    index += layout.rows[r].count;
  }
  return index + selCol;
}

void KeyboardEntryActivity::clampSelection() {
  const fui::KeyboardLayout& layout = currentLayout();
  if (layout.rowCount == 0) {
    selRow = 0;
    selCol = 0;
    return;
  }
  if (selRow < 0) selRow = 0;
  if (selRow >= layout.rowCount) selRow = layout.rowCount - 1;
  const int cols = layout.rows[selRow].count;
  if (selCol < 0) selCol = 0;
  if (selCol >= cols) selCol = cols > 0 ? cols - 1 : 0;
}

void KeyboardEntryActivity::moveSelectionRow(const int delta) {
  const fui::KeyboardLayout& layout = currentLayout();
  if (layout.rowCount == 0) return;
  const int oldCols = selRow < layout.rowCount ? layout.rows[selRow].count : 1;
  selRow = (selRow + delta + layout.rowCount) % layout.rowCount;
  const int newCols = layout.rows[selRow].count;
  // Proportional column mapping keeps vertical travel intuitive between rows
  // of different key counts (e.g. a 10-key letter row over a 6-key bottom row).
  if (oldCols > 0 && newCols > 0 && oldCols != newCols) {
    selCol = selCol * newCols / oldCols;
  }
  clampSelection();
}

void KeyboardEntryActivity::moveSelectionCol(const int delta) {
  const fui::KeyboardLayout& layout = currentLayout();
  if (selRow < 0 || selRow >= layout.rowCount) return;
  const int cols = layout.rows[selRow].count;
  if (cols <= 0) return;
  selCol = (selCol + delta + cols) % cols;
}

bool KeyboardEntryActivity::syncSelectionToValue(const int16_t value) {
  const fui::KeyboardLayout& layout = currentLayout();
  for (int r = 0; r < layout.rowCount; r++) {
    for (int c = 0; c < layout.rows[r].count; c++) {
      if (layout.rows[r].keys[c].value == value) {
        selRow = r;
        selCol = c;
        return true;
      }
    }
  }
  return false;
}

size_t KeyboardEntryActivity::utf8Prev(const std::string& s, size_t pos) {
  if (pos == 0) return 0;
  pos--;
  while (pos > 0 && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80) pos--;
  return pos;
}

size_t KeyboardEntryActivity::utf8Next(const std::string& s, size_t pos) {
  if (pos >= s.length()) return s.length();
  pos++;
  while (pos < s.length() && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80) pos++;
  return pos;
}

void KeyboardEntryActivity::insertUtf8(const char* out) {
  if (!out || !*out) return;
  const size_t n = strlen(out);
  if (maxLength != 0 && text.length() + n > maxLength) return;
  if (cursorPos > text.length()) cursorPos = text.length();
  text.insert(cursorPos, out, n);
  cursorPos += n;
}

bool KeyboardEntryActivity::backspaceUtf8() {
  if (text.empty() || cursorPos == 0) return false;
  const size_t prev = utf8Prev(text, cursorPos);
  text.erase(prev, cursorPos - prev);
  cursorPos = prev;
  return true;
}

// --- Chinese Pinyin entry ----------------------------------------------------
//
// The strip above the keyboard owns everything Chinese: the toggle that enters
// the mode, the Pinyin being composed, and the candidate cells. Chinese mode
// changes how letter keys route (composition instead of text) and pins the
// layout to QWERTY — it adds no key tables, so the SDK layouts stay untouched.

void KeyboardEntryActivity::setChineseMode(const bool enabled) {
  if (enabled == chineseMode || !chineseAvailable) return;
  if (enabled) {
    if (!imeActive) imeActive = ime.begin(pinyin::PinyinDb::shared().tables());
    if (!imeActive) return;  // no PSRAM session: stay in the Latin fallback
    // Pinyin reads Latin letters; whatever script was active, Chinese entry
    // uses the QWERTY table. Leaving restores the user's layout.
    layoutBeforeChinese = layoutId;
    layoutId = fui::KeyboardLayoutId::QwertyEn;
    symbols = false;
    urlPanel = false;
    shifted = false;
    chineseMode = true;
    composition.clear();
    candidateSkip = 0;
    refreshCandidates();
  } else {
    chineseMode = false;
    composition.clear();
    candidateSkip = 0;
    layoutId = layoutBeforeChinese;
    shifted = false;
  }
  clampSelection();
}

bool KeyboardEntryActivity::requestCandidates(const size_t skip) {
  if (!imeActive) return false;
  char before[PRECEDING_MAX + 1];
  const size_t pos = std::min(cursorPos, text.length());
  const size_t n = std::min(pos, PRECEDING_MAX);
  if (n > 0) memcpy(before, text.data() + (pos - n), n);
  before[n] = 0;
  // `nine` stays false: this keyboard has no digit grid, and the engine's T9
  // path is only reachable from one.
  return ime.candidatesPage(composition.c_str(), false, nullptr, before, skip);
}

void KeyboardEntryActivity::refreshCandidates() {
  candidateSkip = 0;
  requestCandidates(0);
}

bool KeyboardEntryActivity::commitCandidateAt(const size_t index) {
  if (!imeActive) return false;
  const pinyin::ImeResult& result = ime.result();
  if (index >= result.count) return false;
  const pinyin::ImeCandidate& item = result.items[index];
  const size_t length = strlen(item.text);
  if (maxLength != 0 && text.length() + length > maxLength) return false;
  insertUtf8(item.text);
  if (item.consume <= composition.size()) composition.erase(0, item.consume);
  refreshCandidates();
  return true;
}

void KeyboardEntryActivity::pageCandidates(const int direction, const size_t visibleCount) {
  if (!imeActive) return;
  const size_t step = visibleCount > 0 ? visibleCount : 1;
  if (direction > 0) {
    if (!requestCandidates(candidateSkip + step) || ime.result().count == 0) {
      requestCandidates(candidateSkip);  // put the current page back
      return;
    }
    candidateSkip += step;
  } else {
    if (candidateSkip == 0) return;
    candidateSkip = candidateSkip > step ? candidateSkip - step : 0;
    requestCandidates(candidateSkip);
  }
}

void KeyboardEntryActivity::layoutStrip(StripLayout& out) const {
  out = StripLayout{};
  if (!chineseAvailable) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect kb = keyboardRect();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int underlineBottom = inputStartY() + inputLineCount() * lineHeight + metrics.verticalSpacing + 4;
  const int height = stripHeight(UiHighDpiProfile::enabled);
  const int bottom = kb.y - KEYBOARD_MIN_GAP;
  const int top = bottom - height;
  // No room between the field and the keys: no strip, and with it no way to
  // pick a candidate, so the toggle does not appear either.
  if (top < underlineBottom + 4) return;
  out.strip = fui::Rect{0, static_cast<int16_t>(top), static_cast<int16_t>(pageWidth),
                        static_cast<int16_t>(height)};

  const int margin = 8;
  const int gap = KEYBOARD_MIN_GAP;
  const int rowHeight = height - 2 * margin;
  const int y = top + margin;

  int cursorX = margin;
  const int rightLimit = pageWidth - margin;
  if (!composition.empty()) {
    // Show the tail of the composition: the letters just typed are the ones
    // the candidates are for.
    const char* shown = composition.c_str();
    const int maxWidth = pageWidth / 4;
    int width = renderer.getTextWidth(UI_12_FONT_ID, shown);
    while (width > maxWidth && *shown) {
      shown++;
      width = renderer.getTextWidth(UI_12_FONT_ID, shown);
    }
    if (*shown) {
      out.compose = fui::Rect{static_cast<int16_t>(cursorX), static_cast<int16_t>(y), static_cast<int16_t>(width),
                              static_cast<int16_t>(rowHeight)};
      out.composeText = shown;
      cursorX += width + gap;
    }
  }
  if (chineseMode && imeActive) {
    const pinyin::ImeResult& result = ime.result();
    const int arrowWidth = rowHeight;
    const int cellsLimit = rightLimit - arrowWidth - gap;
    // A homophone list runs past the built-in CJK coverage, so a cell the
    // active face would draw as tofu is dropped rather than offered. The
    // filter must be all-or-nothing for the strip: a card with no CJK-capable
    // face at all still gets its cells, tofu and all, or nothing would be
    // selectable.
    const auto place = [&](const bool requireGlyphs) {
      out.cellCount = 0;
      int cellX = cursorX + arrowWidth + gap;
      for (size_t i = 0; i < result.count && out.cellCount < StripLayout::MAX_CELLS; i++) {
        const char* text = result.items[i].text;
        if (requireGlyphs && !renderer.canRenderText(UI_12_FONT_ID, text)) continue;
        int width = renderer.getTextWidth(UI_12_FONT_ID, text) + STRIP_CELL_PADDING;
        if (width < STRIP_MIN_CELL_WIDTH) width = STRIP_MIN_CELL_WIDTH;
        if (cellX + width > cellsLimit) break;
        out.cells[out.cellCount] = fui::Rect{static_cast<int16_t>(cellX), static_cast<int16_t>(y),
                                             static_cast<int16_t>(width), static_cast<int16_t>(rowHeight)};
        out.cellItem[out.cellCount] = i;
        out.cellCount++;
        cellX += width + gap;
      }
    };
    place(true);
    if (out.cellCount == 0 && result.count > 0) place(false);
    out.prev = fui::Rect{static_cast<int16_t>(cursorX), static_cast<int16_t>(y), static_cast<int16_t>(arrowWidth),
                         static_cast<int16_t>(rowHeight)};
    out.next = fui::Rect{static_cast<int16_t>(rightLimit - arrowWidth), static_cast<int16_t>(y),
                         static_cast<int16_t>(arrowWidth), static_cast<int16_t>(rowHeight)};
  }
}

void KeyboardEntryActivity::drawStrip(const StripLayout& layout) {
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const auto centeredY = [&](const fui::Rect& rect) { return rect.y + (rect.height - lineHeight) / 2; };
  const auto centeredX = [&](const fui::Rect& rect, const char* text, const int font) {
    return rect.x + (rect.width - renderer.getTextWidth(font, text)) / 2;
  };

  if (layout.composeText != nullptr) {
    const fui::Rect& rect = layout.compose;
    renderer.drawText(UI_12_FONT_ID, rect.x, centeredY(rect), layout.composeText, true);
    // A rule marks the Pinyin as pending input, not committed text.
    renderer.drawLine(rect.x, rect.bottom() - 3, rect.right(), rect.bottom() - 3, 1, true);
  }

  if (chineseMode && imeActive) {
    const pinyin::ImeResult& result = ime.result();
    const bool canPrev = candidateSkip > 0;
    const bool canNext =
        result.hasMore ||
        (layout.cellCount > 0 && layout.cellItem[layout.cellCount - 1] + 1 < result.count);
    if (canPrev) renderer.drawText(SMALL_FONT_ID, centeredX(layout.prev, "<", SMALL_FONT_ID), centeredY(layout.prev), "<");
    if (canNext) renderer.drawText(SMALL_FONT_ID, centeredX(layout.next, ">", SMALL_FONT_ID), centeredY(layout.next), ">");
    for (size_t i = 0; i < layout.cellCount; i++) {
      const fui::Rect& cell = layout.cells[i];
      const char* text = result.items[layout.cellItem[i]].text;
      const int textX = centeredX(cell, text, UI_12_FONT_ID);
      if (i == 0) {
        // The leading candidate is what Space and OK would commit: paint it
        // as chosen, the same inverted style the key layer uses.
        renderer.fillRect(cell.x, cell.y, cell.width, cell.height, true);
        renderer.drawText(UI_12_FONT_ID, textX, centeredY(cell), text, false);
      } else {
        renderer.drawRect(cell.x, cell.y, cell.width, cell.height, true);
        renderer.drawText(UI_12_FONT_ID, textX, centeredY(cell), text, true);
      }
    }
  }
}

bool KeyboardEntryActivity::handleStripTap(const int x, const int y) {
  if (!chineseAvailable) return false;
  StripLayout layout;
  layoutStrip(layout);
  if (layout.strip.empty() || !layout.strip.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) return false;
  if (chineseMode && imeActive) {
    if (layout.prev.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) {
      pageCandidates(-1, layout.cellCount);
      return true;
    }
    if (layout.next.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) {
      pageCandidates(1, layout.cellCount);
      return true;
    }
    for (size_t i = 0; i < layout.cellCount; i++) {
      if (layout.cells[i].contains(static_cast<int16_t>(x), static_cast<int16_t>(y)))
        return commitCandidateAt(layout.cellItem[i]);
    }
  }
  return false;
}

bool KeyboardEntryActivity::activateValue(const int16_t value, const bool longPress) {
  switch (value) {
    case fui::QWERTY_KEY_SHIFT:
      delPressCount = 0;
      hintVisible = false;
      // Letters: case toggle. Symbols: pages between "?123" and "#+=".
      shifted = !shifted;
      clampSelection();
      return true;
    case fui::QWERTY_KEY_MODE:
      delPressCount = 0;
      hintVisible = false;
      if (urlPanel) {
        urlPanel = false;
      } else {
        symbols = !symbols;
        shifted = false;
      }
      clampSelection();
      return true;
    case fui::QWERTY_KEY_LANG: {
      delPressCount = 0;
      hintVisible = false;
      const fui::KeyboardLayoutId nextId = keyboard_layouts::next(layoutId);
      // The non-Latin tables draw the key even with one layout enabled; a
      // full-screen e-ink repaint for an unchanged keyboard costs a second.
      if (nextId == layoutId) return false;
      layoutId = nextId;
      // Shift is per-layer: carrying it across would strand the new layout in
      // upper case. Row widths differ between scripts (Cyrillic runs 12/11/11
      // against Latin's 10/9/9), so the selection has to be re-clamped.
      shifted = false;
      clampSelection();
      return true;
    }
    case URL_PANEL_KEY:
      delPressCount = 0;
      hintVisible = false;
      urlPanel = !urlPanel;
      symbols = false;
      shifted = false;
      clampSelection();
      return true;
    case CHINESE_KEY:
      delPressCount = 0;
      hintVisible = false;
      if (!chineseAvailable) return false;
      setChineseMode(!chineseMode);
      return true;
    case fui::QWERTY_KEY_ENTER:
      // Unconfirmed Pinyin never silently disappears into a saved field.
      if (chineseMode && !composition.empty() && ime.result().count == 0) return false;
      if (chineseMode && !composition.empty()) commitCandidateAt(0);
      onComplete(text);
      return false;
    case fui::QWERTY_KEY_BACKSPACE:
      if (longPress) {
        // The long press still means "start over" — but only over what is
        // being composed; the committed text is one more deliberate press
        // away, so a half-typed Pinyin can never eat the field's contents.
        if (chineseMode && !composition.empty()) {
          composition.clear();
          candidateSkip = 0;
          refreshCandidates();
          return true;
        }
        text.clear();
        cursorPos = 0;
        return true;
      }
      if (chineseMode && !composition.empty()) {
        composition.pop_back();
        candidateSkip = 0;
        refreshCandidates();
        return true;
      }
      delPressCount++;
      if (delPressCount >= 2) {
        hintVisible = true;
        hintShowTime = millis();
      }
      backspaceUtf8();
      return true;
    default: {
      delPressCount = 0;
      hintVisible = false;
      const fui::KeyboardLayout& layer = currentLayout();
      // keyboardAltOutputFor covers explicit alts and the letter case-flip.
      const char* out = longPress ? fui::keyboardAltOutputFor(layer, value) : nullptr;
      if (!out) out = fui::keyboardOutputFor(layer, value);
      if (!out) return false;
      if (chineseMode) {
        // Space commits the leading candidate, exactly like kiikoread's 选择 key;
        // with nothing composing it stays an ordinary space.
        if (value == fui::QWERTY_KEY_SPACE) {
          if (!composition.empty()) return commitCandidateAt(0);
          insertUtf8(" ");
          return true;
        }
        const char c = out[0];
        const bool singleLetter =
            c != 0 && out[1] == 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        if (singleLetter) {
          if (composition.size() < pinyin::kImeRawMax) {
            composition.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            candidateSkip = 0;
            refreshCandidates();
          }
          return true;
        }
      }
      insertUtf8(out);
      if (shifted && !symbols) {
        shifted = false;  // shift auto-releases after one character
        clampSelection();
      }
      return true;
    }
  }
}

bool KeyboardEntryActivity::clearAllOrAltOnSelected() {
  const fui::KeyboardKey* key = selectedKey();
  if (!key) return false;
  if (key->value == fui::QWERTY_KEY_BACKSPACE) {
    text.clear();
    cursorPos = 0;
    return true;
  }
  // Explicit alts and the letter case-flip, same as touch long-press.
  const char* alt = fui::keyboardAltOutputFor(currentLayout(), key->value);
  if (alt) {
    insertUtf8(alt);
    return true;
  }
  return false;
}

std::string KeyboardEntryActivity::displayTextForCurrentState() const {
  std::string displayText = text;
  if (inputType != InputType::Password || passwordVisible) {
    return displayText;
  }

  size_t revealPos;
  if (cursorMode) {
    revealPos = text.length();  // no reveal in displayText; block draws actual char directly
  } else {
    revealPos = (text.length() > 0 && cursorPos > 0) ? cursorPos - 1 : std::string::npos;
  }
  for (size_t i = 0; i < displayText.length(); i++) {
    if (i != revealPos) {
      displayText[i] = '*';
    }
  }
  return displayText;
}

int KeyboardEntryActivity::measureRange(std::string& s, const int start, const int end) const {
  if (end <= start) return 0;
  // s[end] is writable even at s.length() (the terminator slot); only '\0' may
  // be written there, which is exactly what the measurement needs.
  const char saved = s[end];
  s[end] = '\0';
  const int width = renderer.getTextAdvanceX(UI_12_FONT_ID, s.c_str() + start, EpdFontFamily::REGULAR);
  s[end] = saved;
  return width;
}

bool KeyboardEntryActivity::rangeIsRtl(std::string& s, const int start, const int end) const {
  if (end <= start) return false;
  const char saved = s[end];
  s[end] = '\0';
  const bool isRtl = BidiUtils::detectParagraphLevel(s.c_str() + start, 0, end - start) != 0;
  s[end] = saved;
  return isRtl;
}

int KeyboardEntryActivity::lineBreakEnd(std::string& s, const int start, const int maxWidth) const {
  const int len = static_cast<int>(s.length());
  if (measureRange(s, start, len) <= maxWidth) return len;
  int lo = start + 1;
  int hi = len - 1;
  int best = start + 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    if (measureRange(s, start, mid) <= maxWidth) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }

  // The byte-index search can stop inside a character; snap back to a boundary,
  // keeping one whole character so the wrap loop always advances.
  const int firstCharEnd = static_cast<int>(utf8Next(s, static_cast<size_t>(start)));
  while (best > start && (static_cast<uint8_t>(s[best]) & 0xC0) == 0x80) best--;
  // Widths measured mid-character are unreliable, so the search can overshoot.
  while (best > firstCharEnd && measureRange(s, start, best) > maxWidth) {
    best = static_cast<int>(utf8Prev(s, static_cast<size_t>(best)));
  }
  return best < firstCharEnd ? firstCharEnd : best;
}

int KeyboardEntryActivity::inputLineCount() const {
  return UiHighDpiProfile::enabled && renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
}

int KeyboardEntryActivity::inputWindowStart(std::string& displayText, const int width) const {
  if (!UiHighDpiProfile::enabled) return 0;
  int previous = 0;
  int start = 0;
  while (start < static_cast<int>(displayText.size())) {
    const int end = lineBreakEnd(displayText, start, width);
    if (cursorPos < static_cast<size_t>(end) || end == static_cast<int>(displayText.size()))
      return inputLineCount() == 1 ? start : previous;
    previous = start;
    start = end;
  }
  return inputLineCount() == 1 ? start : previous;
}

int KeyboardEntryActivity::inputStartY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UiHighDpiProfile::enabled) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
    return safe.y + metrics.topPadding + metrics.headerHeight + UiHighDpiProfile::controlGap;
  }
  constexpr int MIN_HEADER_GAP = 6;
  const int themedY =
      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 5 + metrics.keyboardVerticalOffset;
  return std::max(themedY, metrics.topPadding + metrics.headerHeight + MIN_HEADER_GAP);
}

bool KeyboardEntryActivity::cursorPositionFromPoint(const int x, const int y, size_t& position,
                                                    bool& passwordToggle) const {
  passwordToggle = false;
  // Key taps are the overwhelmingly common case; they land on the keyboard,
  // never the text field, so skip the wrap/measure work entirely.
  if (y >= keyboardRect().y) return false;

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int inputY = inputStartY();

  int availableWidth = pageWidth;
  if (gpio.deviceIsX3()) {
    availableWidth -= 2 * metrics.sideButtonHintsWidth;
  }
  const int effectiveMargin = (pageWidth - availableWidth * metrics.keyboardTextFieldWidthPercent / 100) / 2;
  const int toggleGap = inputType == InputType::Password ? 4 : 0;
  const int toggleReserve = inputType == InputType::Password ? std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"),
                                                                        renderer.getTextWidth(UI_12_FONT_ID, "[***]")) +
                                                                   toggleGap
                                                             : 0;
  const int textAreaWidth = pageWidth - 2 * effectiveMargin - toggleReserve;
  // The [abc] toggle is drawn one line below the last text line, so that line's
  // hit rect -- which extends verticalSpacing past the glyphs -- overlaps it, and
  // `x >= lineStartX + textWidth` would swallow the tap as an end-of-line cursor
  // placement before the toggle test below ever runs. Masked text is short, so
  // that branch always won. Text never reaches this column (textAreaWidth already
  // reserves it), so excluding the column costs nothing and keeps the two hit
  // regions disjoint.
  const bool hasToggleColumn = (inputType == InputType::Password);
  const int toggleColumnX = pageWidth - effectiveMargin - (toggleReserve - toggleGap) - 4;
  const int maxLineWidth = textAreaWidth;
  const bool centerText = metrics.keyboardCenteredText;
  std::string displayText = displayTextForCurrentState();

  int lineStartIdx = inputWindowStart(displayText, maxLineWidth);
  int lineY = inputY;
  int lastLineStartIdx = 0;
  int lastLineEndIdx = static_cast<int>(displayText.length());
  int lastLineStartX = effectiveMargin;
  int lastLineWidth = 0;

  while (true) {
    if (UiHighDpiProfile::enabled && lineY >= inputY + inputLineCount() * lineHeight) break;
    const int lineEndIdx = lineBreakEnd(displayText, lineStartIdx, maxLineWidth);
    const int textWidth = measureRange(displayText, lineStartIdx, lineEndIdx);
    const int lineStartX = centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2 : effectiveMargin;
    const bool isRtl = rangeIsRtl(displayText, lineStartIdx, lineEndIdx);
    lastLineStartIdx = lineStartIdx;
    lastLineEndIdx = lineEndIdx;
    lastLineStartX = lineStartX;
    lastLineWidth = textWidth;

    if (y >= lineY - metrics.verticalSpacing && y < lineY + lineHeight + metrics.verticalSpacing &&
        !(hasToggleColumn && x >= toggleColumnX)) {
      if (x <= lineStartX) {
        position = static_cast<size_t>(isRtl ? lineEndIdx : lineStartIdx);
        return true;
      }
      if (x >= lineStartX + textWidth) {
        position = static_cast<size_t>(isRtl ? lineStartIdx : lineEndIdx);
        return true;
      }

      int previousWidth = 0;
      for (int i = lineStartIdx; i < lineEndIdx;) {
        const int next = static_cast<int>(utf8Next(displayText, static_cast<size_t>(i)));
        const int nextWidth = measureRange(displayText, lineStartIdx, next);
        const int halfAdvance = (nextWidth - previousWidth) / 2;
        const int midpoint =
            isRtl ? lineStartX + textWidth - previousWidth - halfAdvance : lineStartX + previousWidth + halfAdvance;
        if ((isRtl && x >= midpoint) || (!isRtl && x < midpoint)) {
          position = static_cast<size_t>(i);
          return true;
        }
        previousWidth = nextWidth;
        i = next;
      }
      position = static_cast<size_t>(lineEndIdx);
      return true;
    }

    if (lineEndIdx == static_cast<int>(displayText.length())) {
      break;
    }

    lineY += lineHeight;
    lineStartIdx = lineEndIdx;
  }

  if (inputType == InputType::Password) {
    const int toggleWidth =
        std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"), renderer.getTextWidth(UI_12_FONT_ID, "[***]"));
    const int toggleX = pageWidth - effectiveMargin - toggleWidth;
    if (x >= toggleX - 4 && x < pageWidth - effectiveMargin + 4 && y >= lineY - 2 && y < lineY + lineHeight + 5) {
      passwordToggle = true;
      return true;
    }
  }

  const int underlineBottom = lineY + lineHeight + metrics.verticalSpacing + 8;
  if (y >= inputY - metrics.verticalSpacing && y < underlineBottom && x >= effectiveMargin &&
      x < effectiveMargin + maxLineWidth + toggleReserve) {
    const bool isRtl = rangeIsRtl(displayText, lastLineStartIdx, lastLineEndIdx);
    const bool insideText = x < lastLineStartX + lastLineWidth;
    position = static_cast<size_t>(insideText == isRtl ? lastLineEndIdx : lastLineStartIdx);
    return true;
  }

  return false;
}

fui::Rect KeyboardEntryActivity::keyboardRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int rows = currentLayout().rowCount;
  const bool hasTouch = mappedInput.hasTouch();
  if (UiHighDpiProfile::enabled) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer);
    const int bottom = safe.y + safe.height - metrics.buttonHintsHeight - UiHighDpiProfile::controlGap;
    // Compact landscape keeps the cursor's line visible and spends the saved
    // height on keys. The underline and its 6px gap remain inside the input band.
    const int keyboardInputY =
        UITheme::getInstance().usesModernLayout()
            ? std::max(inputStartY(), safe.y + UpstreamStyleMetrics::values.topPadding + UiHighDpiProfile::headerHeight +
                                          UiHighDpiProfile::controlGap)
            : inputStartY();
    const int inputBottom = keyboardInputY + inputLineCount() * renderer.getLineHeight(UI_12_FONT_ID) +
                            metrics.verticalSpacing + 4 + KEYBOARD_MIN_GAP;
    const int available = bottom - inputBottom;
    const int keyHeight = std::min(
        metrics.keyboardKeyHeight,
        (available - KEYBOARD_PANEL_PADDING * 2 - (rows - 1) * keyboardGap(metrics, inputLineCount() == 1)) / rows);
    const int height =
        rows * keyHeight + (rows - 1) * keyboardGap(metrics, inputLineCount() == 1) + KEYBOARD_PANEL_PADDING * 2;
    return fui::Rect{static_cast<int16_t>(safe.x), static_cast<int16_t>(bottom - height),
                     static_cast<int16_t>(safe.width), static_cast<int16_t>(height)};
  }
  const int height = keyboardKeysHeight(metrics, rows, hasTouch);
  const int hintGap = hasTouch
                          ? (UITheme::getInstance().usesModernLayout() ? UpstreamStyleMetrics::values.verticalSpacing
                                                                       : metrics.verticalSpacing) -
                                metrics.keyboardVerticalOffset
                          : BUTTON_KEYBOARD_HINT_GAP;
  const int y = pageHeight - metrics.buttonHintsHeight - height - hintGap;
  return fui::Rect{0, static_cast<int16_t>(y), static_cast<int16_t>(pageWidth),
                   static_cast<int16_t>(hasTouch ? pageHeight - y : height)};
}

fui::Rect KeyboardEntryActivity::keyboardKeysRect() const {
  const fui::Rect panel = keyboardRect();
  if (UiHighDpiProfile::enabled) return panel;
  const int height =
      keyboardKeysHeight(UITheme::getInstance().getMetrics(), currentLayout().rowCount, mappedInput.hasTouch());
  return fui::Rect{panel.x, static_cast<int16_t>(panel.y + (panel.height - height) / 2), panel.width,
                   static_cast<int16_t>(height)};
}

void KeyboardEntryActivity::loop() {
  int tx = 0;
  int ty = 0;

  size_t touchedCursorPos = 0;
  bool touchedPasswordToggle = false;
  if (mappedInput.wasScreenTapped(tx, ty) && jjwxcSearchFn_ != nullptr && jjwxcSearchRect_.width > 0 &&
      tx >= jjwxcSearchRect_.x && tx < jjwxcSearchRect_.x + jjwxcSearchRect_.width &&
      ty >= jjwxcSearchRect_.y && ty < jjwxcSearchRect_.y + jjwxcSearchRect_.height) {
    jjwxcSearchFn_(jjwxcSearchCtx_, text.c_str());
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    // The candidate strip sits between the field and the keys; its taps are
    // consumed here so they never fall through to cursor placement or the key
    // router below.
    if (handleStripTap(tx, ty)) {
      requestUpdate();
      return;
    }
  }
  if (mappedInput.wasScreenTapped(tx, ty) && cursorPositionFromPoint(tx, ty, touchedCursorPos, touchedPasswordToggle)) {
    if (touchedPasswordToggle) {
      passwordVisible = !passwordVisible;
      cursorMode = false;
      togglePos = false;
      hintVisible = false;
      touchRouter.reset();
      requestUpdate();
      return;
    }
    cursorPos = std::min(touchedCursorPos, text.length());
    // The masked text field maps taps per byte; snap back to a boundary so
    // the cursor never lands inside a multi-byte character.
    while (cursorPos > 0 && cursorPos < text.length() && (static_cast<uint8_t>(text[cursorPos]) & 0xC0) == 0x80) {
      cursorPos--;
    }
    cursorMode = false;
    togglePos = false;
    hintVisible = false;
    touchRouter.reset();
    requestUpdate();
    return;
  }

  if (!cursorMode && interactionsReady) {
    const bool pressedDown = mappedInput.wasScreenTouchDown(tx, ty);
    int tapX = 0;
    int tapY = 0;
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    int hx = 0;
    int hy = 0;
    const bool inContact = mappedInput.isScreenTouchHeld(hx, hy);

    const fui::TouchHoldRouter::Result result =
        touchRouter.update(interactions, pressedDown, static_cast<int16_t>(tx), static_cast<int16_t>(ty), tapped,
                           static_cast<int16_t>(tapX), static_cast<int16_t>(tapY), inContact, millis());
    if (result.event) {
      syncSelectionToValue(result.event.value);
      if (activateValue(result.event.value, result.event.longPress)) {
        requestUpdate();
      }
      return;
    }
    if (result.activeChanged) {
      requestUpdate();
    }
    if (pressedDown || tapped) {
      return;
    }
  }

  if (!cursorMode && mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    upHeld = true;
    upLongHandled = false;
  }

  if (upHeld && !upLongHandled && mappedInput.isPressed(MappedInputManager::Button::Up) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    cursorMode = true;
    upLongHandled = true;
    hintVisible = true;
    hintShowTime = millis();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (upHeld && !upLongHandled && !cursorMode) {
      moveSelectionRow(-1);
      requestUpdate();
    }
    upHeld = false;
    upLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    downHeld = true;
    if (cursorMode) {
      togglePos = false;
      passwordVisible = false;
      cursorMode = false;
      hintVisible = false;
      downLongHandled = true;
      requestUpdate();
    } else {
      downLongHandled = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (downHeld && !downLongHandled && !cursorMode) {
      moveSelectionRow(1);
      requestUpdate();
    }
    downHeld = false;
    downLongHandled = false;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    if (cursorMode) return;
    moveSelectionCol(-1);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorMode) {
      if (togglePos) {
        cursorPos = savedCursorPos;
        togglePos = false;
        requestUpdate();
      } else if (cursorPos > 0) {
        cursorPos = utf8Prev(text, cursorPos);
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      rightHeld = true;
      rightLongHandled = false;
      rightStartCursorPos = cursorPos;
    }
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    if (cursorMode) return;
    moveSelectionCol(1);
    requestUpdate();
  });

  if (rightHeld && !rightLongHandled && mappedInput.isPressed(MappedInputManager::Button::Right) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      savedCursorPos = rightStartCursorPos;
      togglePos = true;
      rightLongHandled = true;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password) {
      rightHeld = false;
      rightLongHandled = false;
    }
    if (cursorMode && !togglePos && cursorPos < text.length()) {
      cursorPos = utf8Next(text, cursorPos);
      requestUpdate();
    }
    if (cursorMode) return;
    rightHeld = false;
    rightLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld = true;
    confirmLongHandled = false;
  }

  const fui::KeyboardKey* selKey = selectedKey();
  const bool selectedDel = selKey && selKey->value == fui::QWERTY_KEY_BACKSPACE;

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > DEL_LONG_PRESS_MS && selectedDel) {
    clearAllOrAltOnSelected();
    confirmLongHandled = true;
    requestUpdate();
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (!selectedDel && clearAllOrAltOnSelected()) {
      requestUpdate();
      confirmLongHandled = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmHeld && !confirmLongHandled && !cursorMode) {
      if (selKey && activateValue(selKey->value, false)) {
        requestUpdate();
      }
    } else if (confirmHeld && !confirmLongHandled && cursorMode && inputType == InputType::Password && togglePos) {
      passwordVisible = !passwordVisible;
      requestUpdate();
    }
    confirmHeld = false;
    confirmLongHandled = false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onCancel();
  }

  if (hintVisible && !cursorMode && millis() - hintShowTime > 4000) {
    hintVisible = false;
    requestUpdate();
  }
}

void KeyboardEntryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int inputY = inputStartY();
  int inputHeight = 0;

  std::string displayText = displayTextForCurrentState();

  const bool isPassword = (inputType == InputType::Password);
  int availableWidth = pageWidth;
  if (gpio.deviceIsX3()) {
    availableWidth -= 2 * metrics.sideButtonHintsWidth;
  }
  const int effectiveMargin = (pageWidth - availableWidth * metrics.keyboardTextFieldWidthPercent / 100) / 2;
  const int toggleGap = isPassword ? 4 : 0;
  const int toggleReserve = isPassword ? std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"),
                                                  renderer.getTextWidth(UI_12_FONT_ID, "[***]")) +
                                             toggleGap
                                       : 0;
  const int textAreaWidth = pageWidth - 2 * effectiveMargin - toggleReserve;
  const int maxLineWidth = textAreaWidth;
  const bool centerText = metrics.keyboardCenteredText;

  // The cursor spans a whole code point: a lone byte of it renders as a replacement glyph.
  // Masking is per byte, so displayText keeps text's length and the same span applies to both.
  const size_t cursorCharBytes = (cursorPos < text.length()) ? utf8Next(text, cursorPos) - cursorPos : 0;
  char cursorChar[8] = {};         // the character under the cursor
  char displayCursorChar[8] = {};  // same span of displayText, masked for passwords
  if (cursorCharBytes > 0) {
    const size_t n = std::min(cursorCharBytes, sizeof(cursorChar) - 1);
    memcpy(cursorChar, text.data() + cursorPos, n);
    memcpy(displayCursorChar, displayText.data() + cursorPos, n);
  }

  int cursorCharWidth = 6;
  if (cursorCharBytes > 0) {
    int w = renderer.getTextWidth(UI_12_FONT_ID, cursorChar);
    if (w > cursorCharWidth) cursorCharWidth = w;
  }

  int lineStartIdx = inputWindowStart(displayText, maxLineWidth);
  int textWidth = 0;
  int cursorPixelX = effectiveMargin;
  int cursorLineY = inputY;
  bool cursorDrawn = false;

  while (true) {
    if (UiHighDpiProfile::enabled && inputHeight >= inputLineCount() * lineHeight) break;
    const int lineEndIdx = lineBreakEnd(displayText, lineStartIdx, maxLineWidth);
    const std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    textWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, lineText.c_str(), EpdFontFamily::REGULAR);
    {
      const bool isRtl = rangeIsRtl(displayText, lineStartIdx, lineEndIdx);
      const int lineStartX = centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2 : effectiveMargin;
      const bool isLastLine = (lineEndIdx == static_cast<int>(displayText.length()));
      bool isCursorLine = false;
      if (!cursorDrawn && cursorPos >= lineStartIdx &&
          (isLastLine ? cursorPos <= lineEndIdx : cursorPos < lineEndIdx)) {
        std::string beforeCursor;
        if (isPassword && !passwordVisible && cursorMode) {
          beforeCursor = std::string(cursorPos - lineStartIdx, '*');
        } else {
          beforeCursor = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        }
        int beforeWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, beforeCursor.c_str(), EpdFontFamily::REGULAR);
        int throughCursorWidth = beforeWidth;
        int kernOffset = 0;
        if (cursorCharBytes > 0) {
          std::string beforeAndCursor = beforeCursor + displayCursorChar;
          throughCursorWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, beforeAndCursor.c_str(), EpdFontFamily::REGULAR);
          int charAdvance = renderer.getTextAdvanceX(UI_12_FONT_ID, displayCursorChar, EpdFontFamily::REGULAR);
          kernOffset = throughCursorWidth - beforeWidth - charAdvance;
        }
        if (isRtl) {
          const int logicalWidth = cursorMode && cursorCharBytes > 0 ? throughCursorWidth : beforeWidth;
          cursorPixelX = lineStartX + textWidth - logicalWidth;
        } else {
          cursorPixelX = lineStartX + beforeWidth + kernOffset;
        }
        cursorLineY = inputY + inputHeight;
        cursorDrawn = true;
        isCursorLine = true;
      }

      if (isCursorLine && cursorMode && isPassword && !passwordVisible && !togglePos) {
        // Draw text in 3 parts to avoid block cursor overflowing onto next char.
        // displayText uses '*' for all chars; actual char may be wider than '*'.
        // Part 1: chars before cursor position
        const std::string part1 = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        renderer.drawText(UI_12_FONT_ID, lineStartX, inputY + inputHeight, part1.c_str());
        // Part 2: skip cursor slot (block + actual char drawn later)
        // Part 3: chars after cursor position (skip char under cursor), starting at cursorPixelX + cursorCharWidth
        const int afterStart = static_cast<int>(cursorPos + cursorCharBytes);
        const int afterEnd = lineEndIdx;
        if (afterStart < afterEnd) {
          const std::string part3 = displayText.substr(afterStart, afterEnd - afterStart);
          renderer.drawText(UI_12_FONT_ID, cursorPixelX + cursorCharWidth, inputY + inputHeight, part3.c_str());
        }
      } else {
        renderer.drawText(UI_12_FONT_ID, lineStartX, inputY + inputHeight, lineText.c_str());
      }
      if (lineEndIdx == static_cast<int>(displayText.length())) {
        break;
      }

      inputHeight += lineHeight;
      lineStartIdx = lineEndIdx;
    }
  }

  const int fieldWidth = (inputHeight > 0) ? maxLineWidth : textWidth;
  const int lineMargin = effectiveMargin;
  GUI.drawTextField(renderer, Rect{0, inputY, pageWidth, inputHeight}, fieldWidth, cursorMode, lineMargin,
                    pageWidth - 2 * lineMargin);

  if (cursorMode && !togglePos && cursorPos <= displayText.length()) {
    static constexpr int blockPadding = 1;
    renderer.fillRect(cursorPixelX - blockPadding, cursorLineY, cursorCharWidth + blockPadding * 2, lineHeight, true);
    if (cursorCharBytes > 0) {
      renderer.drawText(UI_12_FONT_ID, cursorPixelX, cursorLineY, cursorChar, false);
    }
  } else if (cursorPos <= displayText.length()) {
    static constexpr int serifW = 3;
    const int cX = cursorPixelX;
    const int cY = cursorLineY;
    const int cBottom = cursorLineY + lineHeight - 1;
    renderer.fillRect(cX, cY, 2, lineHeight, true);
    renderer.drawLine(cX - serifW, cY, cX - 1, cY, 2, true);
    renderer.drawLine(cX + 1, cY, cX + serifW, cY, 2, true);
    renderer.drawLine(cX - serifW, cBottom, cX - 1, cBottom, 2, true);
    renderer.drawLine(cX + 1, cBottom, cX + serifW, cBottom, 2, true);
  }

  if (isPassword) {
    const char* toggleLabel = passwordVisible ? "[***]" : "[abc]";
    const int toggleWidth = renderer.getTextWidth(UI_12_FONT_ID, toggleLabel);
    const int toggleX = pageWidth - effectiveMargin - toggleWidth;
    const int toggleY = inputY + inputHeight;
    const bool toggleSelected = cursorMode && togglePos;

    if (toggleSelected) {
      renderer.fillRect(toggleX - 2, toggleY, toggleWidth + 5, lineHeight + 3, true);
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, false);
    } else {
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, true);
    }
  }

  if ((!UITheme::getInstance().hasMainTabs() || !mappedInput.hasTouch()) && hintVisible && !text.empty()) {
    const int hintLh = renderer.getLineHeight(SMALL_FONT_ID);
    const int underlineY = inputY + inputHeight + lineHeight + metrics.verticalSpacing;
    const int hintY = underlineY + 4;
    if (cursorMode) {
      int hintLineY = hintY;
      if (inputType == InputType::Password && togglePos) {
        renderer.drawCenteredText(
            SMALL_FONT_ID, hintLineY,
            passwordVisible ? tr(STR_KB_HINT_TOGGLE_HIDE_PASSWORD) : tr(STR_KB_HINT_TOGGLE_SHOW_PASSWORD), true);
        hintLineY += hintLh;
        renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, tr(STR_KB_HINT_RETURN_CURSOR), true);
      } else {
        renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, tr(STR_KB_HINT_MOVE_CURSOR), true);
        hintLineY += hintLh;
        if (inputType == InputType::Password) {
          const char* passTip = passwordVisible ? tr(STR_KB_HINT_HIDE_PASSWORD) : tr(STR_KB_HINT_SHOW_PASSWORD);
          renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, passTip, true);
        }
      }
    } else {
      renderer.drawCenteredText(SMALL_FONT_ID, hintY, tr(STR_KB_HINT_EDIT_ENTRY), true);
    }
  }

  const fui::Rect kbRect = keyboardRect();
  const fui::Rect keysRect = keyboardKeysRect();

  // The Chinese strip (toggle + composing Pinyin + candidates) lives in the
  // band between the field and the keys and owns it while it is up.
  StripLayout strip;
  layoutStrip(strip);
  if (!strip.strip.empty()) drawStrip(strip);

  // refreshed UI frame 07: a live result preview above the QWERTY keys. The preview
  // is local-only (the JJWXC search runs in its own app); the row at the bottom
  // says so instead of pretending to merge two result sets.
  if (liveResultsFn_ != nullptr && !chineseMode && !text.empty()) {
    const int previewTop = inputY + inputHeight + lineHeight + metrics.verticalSpacing + 10;
    const int previewBottom = kbRect.y - 8;
    if (previewBottom - previewTop >= 80) {
      const int rowH = std::min(64, std::max(44, (previewBottom - previewTop - 32) / 5));
      int y = previewTop;
      renderer.drawText(SMALL_FONT_ID, effectiveMargin, y, tr(STR_LOCAL_MATCHES), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(SMALL_FONT_ID) + 4;
      const auto matches = liveResultsFn_(liveResultsCtx_, text.c_str());
      if (matches.empty()) {
        renderer.drawText(UI_10_FONT_ID, effectiveMargin, y, tr(STR_LOCAL_NO_MATCHES));
      } else {
        for (size_t i = 0; i < matches.size() && y + rowH <= previewBottom - 24; ++i) {
          renderer.drawText(UI_10_FONT_ID, effectiveMargin, y + (rowH - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                            renderer.truncatedText(UI_10_FONT_ID, matches[i].c_str(),
                                                   pageWidth - effectiveMargin * 2)
                                .c_str());
          y += rowH;
        }
      }
      if (jjwxcSearchFn_ != nullptr) {
        const int lineY = std::min(y + 6, previewBottom - renderer.getLineHeight(SMALL_FONT_ID));
        renderer.drawText(SMALL_FONT_ID, effectiveMargin, lineY, tr(STR_JJWXC_SEARCH_IN_APP));
        jjwxcSearchRect_ = freeink::ui::Rect{static_cast<int16_t>(effectiveMargin),
                                             static_cast<int16_t>(lineY - 6),
                                             static_cast<int16_t>(pageWidth - effectiveMargin * 2),
                                             static_cast<int16_t>(renderer.getLineHeight(SMALL_FONT_ID) + 12)};
        renderer.drawLine(pageWidth - effectiveMargin - 8, lineY + 6, pageWidth - effectiveMargin, lineY + 12, true);
        renderer.drawLine(pageWidth - effectiveMargin, lineY + 12, pageWidth - effectiveMargin - 8, lineY + 18, true);
      } else {
        jjwxcSearchRect_ = freeink::ui::Rect{};
      }
    }
  }

  const int tipsLh = renderer.getLineHeight(SMALL_FONT_ID);
  const int underlineBottom = inputY + inputHeight + lineHeight + metrics.verticalSpacing + 4;
  auto drawTip = [&](const char* tip, int y) { renderer.drawCenteredText(SMALL_FONT_ID, y, tip, true); };

  int tipCount = 0;
  if (cursorMode) {
    tipCount = 1;
  } else if (urlPanel) {
    tipCount = 1 + (!text.empty() ? 1 : 0);
  } else if (symbols) {
    tipCount = !text.empty() ? 1 : 0;
  } else {
    tipCount = 1 + (inputType == InputType::Url ? 1 : 0) + (!text.empty() ? 1 : 0);
  }

  if (strip.strip.empty() && (!UITheme::getInstance().hasMainTabs() || !mappedInput.hasTouch()) && tipCount > 0) {
    int y = (underlineBottom + kbRect.y) / 2 - (tipCount + 1) * tipsLh / 2;
    drawTip(tr(STR_KB_TIPS), y);
    y += tipsLh;
    if (cursorMode) {
      drawTip(tr(STR_KB_HINT_RETURN_KEYBOARD), y);
    } else if (urlPanel) {
      drawTip(tr(STR_KB_HINT_EXIT_URL_MODE), y);
      y += tipsLh;
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    } else if (symbols) {
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    } else {
      const char* altCharTip;
      if (inputType == InputType::Url) {
        altCharTip = tr(STR_KB_HINT_SECONDARY_CHAR);
      } else if (shifted) {
        altCharTip = tr(STR_KB_HINT_LOWER_SECONDARY);
      } else {
        altCharTip = tr(STR_KB_HINT_UPPER_SECONDARY);
      }
      drawTip(altCharTip, y);
      y += tipsLh;
      if (inputType == InputType::Url) {
        drawTip(tr(STR_KB_HINT_URL_SNIPPETS), y);
        y += tipsLh;
      }
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    }
  }

  // The FreeInkUI keyboard draws the keys and registers their hit rects into
  // `interactions`; loop() (the main task) routes touch snapshots against
  // that table via TouchHoldRouter, which reads the published generation
  // (routePublished()/publishedData()) — beginPublishCycle() here makes this
  // render build into the OTHER generation, so loop() never sees a
  // half-rebuilt table no matter when it runs relative to this.
  interactions.beginPublishCycle();
  fui::GfxRendererTarget target(renderer);
  target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  fui::Frame<56> frame(target, device, noInput, interactions);

  fui::KeyboardProps props;
  const fui::KeyboardLayout& layout = currentLayout();
  props.layout = &layout;
  props.keyAction = ACTION_KEY;  // one action id; loop() dispatches on key value
  props.okLabel = tr(STR_OK_BUTTON);
  // Match the label to the layer the mode key leads back from: the symbols
  // layer and the URL snippet panel both label it "abc" in the static tables.
  props.modeLabel =
      (symbols || (inputType == InputType::Url && urlPanel)) ? tr(STR_KEY_MODE_ABC) : tr(STR_KEY_MODE_SYMBOLS);
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
  props.selectedIndex = (cursorMode || mappedInput.hasTouch()) ? -1 : static_cast<int16_t>(selectedLogicalIndex());
  if (!cursorMode && mappedInput.hasTouch() && chineseAvailable && chineseMode) {
    // Touch navigation never marks a key, so the slot stays -1 and the Chinese
    // mode would have no visible state. Pin it to the globe: the selected
    // style draws it inverted, the same "this one is on" idiom every other
    // control in this app uses.
    props.selectedIndex = static_cast<int16_t>(globeLogicalIndex());
  }
  // The 12-column Arabic rows need the smaller font for wide isolated letters.
  props.labelText.font = layoutId == fui::KeyboardLayoutId::ArabicAr && !symbols ? fui::GfxRendererTarget::FONT_SMALL
                                                                                 : fui::GfxRendererTarget::FONT_BODY;
  props.altText.font = fui::GfxRendererTarget::FONT_SMALL;
  if (UiHighDpiProfile::enabled) {
    const int rows = layout.rowCount;
    const int keyHeight =
        (keysRect.height - KEYBOARD_PANEL_PADDING * 2 - (rows - 1) * keyboardGap(metrics, inputLineCount() == 1)) /
        rows;
    if (keyHeight < renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 8)
      props.labelText.font = fui::GfxRendererTarget::FONT_SMALL;
  }
  props.gap = props.rowGap =
      static_cast<int16_t>(keyboardGap(metrics, UiHighDpiProfile::enabled && inputLineCount() == 1));
  if (urlPanel) props.uniformKeyWidth = false;  // Shortcut rows stretch independently.
  if (!mappedInput.hasTouch()) props.background = fui::Paint::none();
  frame.target().fill(kbRect, props.background);
  const int bottomEdge =
      mappedInput.hasTouch() ? renderer.getScreenHeight() : renderer.getScreenHeight() - metrics.buttonHintsHeight;
  props.bottomHitOverflow =
      UiHighDpiProfile::enabled ? 0 : static_cast<int16_t>(std::max(0, bottomEdge - keysRect.bottom()));
  fui::keyboard(frame, keysRect, props);
  interactions.publish();
  interactionsReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  GUI.drawSideButtonHints(renderer, ">", "<");

  // TransientUi: a keystroke repaints the whole surface, and on Read Pico every
  // push is a full-page differential scan that counts toward the periodic GC16
  // sweep -- so typing flashed black/white every Nth key, mid-word. The keyboard
  // is a short-lived modal; excluding its pushes from the sweep keeps typing
  // clean, and the screen it closes into resumes Normal pushes (and the sweep)
  // right after, clearing any faint residue from the text field.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, DisplayRefreshContext::TransientUi);
}

void KeyboardEntryActivity::onComplete(std::string text) {
  setResult(KeyboardResult{std::move(text)});
  finish();
}

void KeyboardEntryActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
