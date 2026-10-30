#pragma once
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <PinyinDb.h>
#include <PinyinIme.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class InputType {
  Text,  // free-form text; Chinese entry is offered where the platform supports it
  // Accounts, device codes, numbers: the plain keyboard, and never the
  // Chinese toggle -- a field whose content is ASCII by construction has no
  // use for Pinyin, and the narrower keys are worth more than the option.
  Ascii,
  Password,
  Url,
};

// Text entry on the FreeInkUI keyboard component: the SDK layout tables and
// keyboard() do the key rendering and hit-rect registration, InteractionBuffer
// routes taps/long-presses, and this activity owns the text field, cursor
// editing, and the URL snippet layouts.
class KeyboardEntryActivity : public Activity {
 public:
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, InputType inputType = InputType::Text)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        inputType(inputType) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Serial-only soak hook: inserts one character through the same path a touch
  // key press takes, so band-update behaviour can be exercised without a finger.
  void debugTypeChar() override;
  bool wantsBandUpdates() const override { return true; }
  bool preventAutoSleep() override { return true; }

  // Optional live preview above the keyboard (library search). The callback
  // returns the first few matching titles for the current query; it is called
  // from render, so it must be cheap and must not touch SD.
  using LiveResultsFn = std::vector<std::string> (*)(void* ctx, const char* query);
  using JjwxcSearchFn = void (*)(void* ctx, const char* query);
  void setLiveResults(LiveResultsFn fn, void* ctx);
  void setJjwxcSearch(JjwxcSearchFn fn, void* ctx) {
    jjwxcSearchFn_ = fn;
    jjwxcSearchCtx_ = ctx;
  }

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  InputType inputType;
  bool passwordVisible = false;

  // --- Chinese Pinyin entry -------------------------------------------------
  // Available when a Text field has the SD Pinyin database, PSRAM to hold it,
  // and a touch panel to drive the candidate strip (the strip is the only way
  // to choose a candidate, so no touch means no Chinese mode). English input
  // is the untouched fallback in every other case.
  bool chineseAvailable = false;
  bool chineseMode = false;
  bool imeActive = false;
  freeink::ui::KeyboardLayoutId layoutBeforeChinese = freeink::ui::KeyboardLayoutId::QwertyEn;

  // The globe key spliced into the bottom row while Chinese entry is
  // available, in the slot left of the mode key. Kind Lang draws the SDK's
  // globe glyph; the value is app-owned so activateValue() can claim it.
  static constexpr int16_t CHINESE_KEY = -5;
  // Caller-owned copy of the active layer with that key spliced in, rebuilt
  // whenever the base layer changes (baseLayout() returns statics, so the
  // pointer is a complete cache key).
  mutable freeink::ui::KeyboardLayout chineseLayout_{};
  mutable freeink::ui::KeyboardRow chineseRowCache_[6] = {};
  mutable freeink::ui::KeyboardKey chineseKeyCache_[8] = {};
  mutable const freeink::ui::KeyboardLayout* chineseBaseCache = nullptr;
  // Raw lowercase Pinyin not yet committed; always a prefix of what the strip
  // is showing candidates for.
  std::string composition;
  pinyin::PinyinIme ime;
  // First item of the current candidate page (the engine's `skip`).
  size_t candidateSkip = 0;

  // The strip's geometry, computed once per paint and once per tap from the
  // same measurements so drawing and hit-testing cannot disagree.
  struct StripLayout {
    freeink::ui::Rect strip{};
    freeink::ui::Rect compose{};
    const char* composeText = nullptr;  // tail of `composition` that fits
    freeink::ui::Rect prev{};
    freeink::ui::Rect next{};
    static constexpr size_t MAX_CELLS = 8;
    freeink::ui::Rect cells[MAX_CELLS];
    size_t cellItem[MAX_CELLS] = {};  // index into the IME result per cell
    size_t cellCount = 0;
  };

  void layoutStrip(StripLayout& out) const;
  void drawStrip(const StripLayout& layout);
  // Ask the engine for a page starting at `skip`, with the committed text
  // before the caret as the continuation context.
  bool requestCandidates(size_t skip);
  void refreshCandidates();
  bool commitCandidateAt(size_t index);
  void pageCandidates(int direction, size_t visibleCount);
  bool handleStripTap(int x, int y);
  void setChineseMode(bool enabled);


  LiveResultsFn liveResultsFn_ = nullptr;
  void* liveResultsCtx_ = nullptr;
  JjwxcSearchFn jjwxcSearchFn_ = nullptr;
  void* jjwxcSearchCtx_ = nullptr;
  freeink::ui::Rect jjwxcSearchRect_{};
  ButtonNavigator buttonNavigator;

  // Keyboard layers. The letter/symbol layers come from the SDK's builtin
  // layouts (with the always-visible number row); the URL layers are
  // app-defined tables in the .cpp.
  freeink::ui::KeyboardLayoutId layoutId = freeink::ui::KeyboardLayoutId::QwertyEn;
  // Asks the SDK for a layout variant with the language key. Only the Latin
  // layouts honour it; the Cyrillic, Hebrew and Arabic tables carry the key
  // either way.
  // Resolved once in onEnter(): the set cannot change while a keyboard is on
  // screen, and currentLayout() runs on every loop pass.
  bool showLangKey = false;
  bool shifted = false;
  bool symbols = false;
  bool urlPanel = false;  // URL snippet panel replaces the letter layer

  // Key hit rects registered by the keyboard component during render(); loop()
  // routes touch snapshots against them. The 5-row EN layout registers 41 keys,
  // Cyrillic's wider rows (12/11/11 against 10/9/9) exactly 48, so 56 restores
  // the headroom. Two generations of 16-byte entries: 256 bytes for the eight
  // extra slots, no heap.
  freeink::ui::InteractionBuffer<56> interactions;

  // GPIO selection over the current layout grid (row/col in layout terms;
  // the bottom action row is just the last row).
  int selRow = 0;
  int selCol = 0;

  bool confirmHeld = false;
  bool confirmLongHandled = false;

  bool cursorMode = false;
  bool togglePos = false;
  size_t cursorPos = 0;  // byte offset into text (always on a code point boundary)
  bool upHeld = false;
  bool upLongHandled = false;
  bool downHeld = false;
  bool downLongHandled = false;
  bool rightHeld = false;
  bool rightLongHandled = false;
  size_t savedCursorPos = 0;
  size_t rightStartCursorPos = 0;

  // Tap/hold routing (threshold long-press, release swallow, slide re-arm)
  // lives in the SDK; loop() feeds it the level-triggered touch state.
  freeink::ui::TouchHoldRouter touchRouter;

  // loop() runs on the main task while render() rebuilds the interaction
  // table on the render task; render() opts `interactions` into the SDK's
  // double-buffered publish cycle (beginPublishCycle()/publish()) so
  // TouchHoldRouter's routePublished()-based reads in loop() always see a
  // complete, previously-published table, never one mid-rebuild — no taps
  // dropped for that reason anymore. interactionsReady itself gates only
  // before the very first publish (nothing registered yet). Do not clear it
  // for later renders: the point of retaining the published generation is
  // that loop() can keep routing a release while the next frame is built.
  // atomic (not volatile) so the flag also orders the first table publication
  // on dual-core targets.
  std::atomic<bool> interactionsReady{false};

  int delPressCount = 0;
  bool hintVisible = false;
  unsigned long hintShowTime = 0;

  void onComplete(std::string text);
  void onCancel();
  int inputStartY() const;
  int inputLineCount() const;
  bool cursorPositionFromPoint(int x, int y, size_t& position, bool& passwordToggle) const;
  std::string displayTextForCurrentState() const;
  // Advance of s[start, end) measured in place by temporarily null-terminating
  // at `end` — avoids a substr temporary per measurement.
  int measureRange(std::string& s, int start, int end) const;
  bool rangeIsRtl(std::string& s, int start, int end) const;
  // Largest line end in (start, s.length()] whose advance fits maxWidth.
  // Binary search over the monotonic prefix advance; always advances at least
  // one byte so an oversized glyph cannot stall the wrap loop.
  int lineBreakEnd(std::string& s, int start, int maxWidth) const;

  // The layer the SDK builds for the current state, as-is.
  const freeink::ui::KeyboardLayout& baseLayout() const;
  // baseLayout(), plus the globe key when Chinese entry is available. Drawing,
  // hit-testing and button navigation all read this one function.
  const freeink::ui::KeyboardLayout& currentLayout() const;
  // Logical index of the globe key (first key of the last row) in
  // currentLayout(), or -1 when there is no such row.
  int globeLogicalIndex() const;
  const freeink::ui::KeyboardKey* selectedKey() const;
  int selectedLogicalIndex() const;
  void clampSelection();
  void moveSelectionRow(int delta);
  void moveSelectionCol(int delta);
  bool syncSelectionToValue(int16_t value);
  // Handles one key activation (by stable key id). Returns true when the
  // screen needs a repaint; OK/cancel finish the activity instead.
  bool activateValue(int16_t value, bool longPress);
  bool clearAllOrAltOnSelected();

  void insertUtf8(const char* out);
  bool backspaceUtf8();
  static size_t utf8Prev(const std::string& s, size_t pos);
  static size_t utf8Next(const std::string& s, size_t pos);

  freeink::ui::Rect keyboardRect() const;
  freeink::ui::Rect keyboardKeysRect() const;
  int inputWindowStart(std::string& displayText, int width) const;

  static constexpr uint16_t LONG_PRESS_MS = 500;
  static constexpr uint16_t DEL_LONG_PRESS_MS = 1500;
  static constexpr uint16_t TOUCH_LONG_PRESS_MS = 350;
  static constexpr uint16_t TOUCH_DEL_LONG_PRESS_MS = 900;

  // App-specific key id: toggles the URL snippet panel (URL fields only).
  static constexpr int16_t URL_PANEL_KEY = -3;
};
