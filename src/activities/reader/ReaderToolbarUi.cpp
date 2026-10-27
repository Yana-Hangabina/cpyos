#include "ReaderToolbarUi.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/jjwxcTocIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/readerToolbarIcons.h"
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
#include "components/icons/readerToolbarIcons48.h"
#endif

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_DISMISS = 1;  // tap anywhere on the page above the sheet
constexpr fui::ActionId ACTION_TOOL = 2;     // value = 0 Contents, 1 Text, 2 More
constexpr fui::ActionId ACTION_PREV = 3;     // scrub row: previous chapter
constexpr fui::ActionId ACTION_NEXT = 4;     // scrub row: next chapter
constexpr fui::ActionId ACTION_SCRUB = 5;    // progress track: dragPermille along the book
constexpr fui::ActionId ACTION_ROW = 6;      // panel list row, value = row index
// Text panel's grid steppers. They map onto the reader's existing row-edge
// stepping by replaying a synthetic touch x (left third / right third), so the
// panel keeps one stepping implementation with the physical Left/Right keys.
constexpr fui::ActionId ACTION_STEP_MINUS = 7;
constexpr fui::ActionId ACTION_STEP_PLUS = 8;
// UiAppHost's UiApp has only eight action-handler slots and the toolbar already
// uses 1..8. A ninth ActionId is silently dropped by FreeInkApp::on(), which is
// why chip taps did nothing. Reuse the registered ACTION_ROW with a high-bit
// marker in its value instead: real row indexes are < 300, so 0x4000 cannot
// collide with a row.
constexpr int16_t kChipActionFlag = 0x4000;
// The fixed Contents maintenance strip reuses ACTION_ROW too; real row indexes
// and the chip marker never include this bit.
constexpr int16_t kUpdateActionFlag = 0x2000;
// The fixed Bookmarks strip (add/remove page bookmark) uses the same trick.
constexpr int16_t kBookmarkActionFlag = 0x1000;
// Bookmarks-panel chips and the annotation export strip. Separate bits because a
// single ACTION_ROW handler carries all of them (see the handler-overflow note).
constexpr int16_t kBookmarkChipFlag = 0x0800;
constexpr int16_t kAnnotationChipFlag = 0x0400;
constexpr int16_t kExportActionFlag = 0x0200;
constexpr int16_t kContentsFilterFlag = 0x0100;
// Type-panel "Done" pill. Only read while the Text panel is open (cell
// indexes are 0..7), so this bit cannot be confused with a list row.
constexpr int16_t kTextDoneFlag = 0x0080;
// Height of the chips row above the Bookmarks list.
constexpr int16_t kBookmarkChipsH = UiHighDpiProfile::enabled ? 56 : 36;

constexpr int kTextTool = 1;
constexpr int kToolTabs = 4;
// Contents tail badge/action and current-chapter bar. The placeholder below
// reserves the slot inside the shared list without touching FreeInkUI's
// ListItem layout. A row carries a 16 px state glyph + its label, then one
// 56x56 rounded action button (refreshed UI frame 06).
constexpr jjwxc_toc::Density kContentsDensity =
    UiHighDpiProfile::enabled ? jjwxc_toc::Density::High : jjwxc_toc::Density::Low;
constexpr int16_t kContentsButton = UiHighDpiProfile::enabled ? 56 : 44;
constexpr int16_t kContentsIcon = jjwxc_toc::actionSize(kContentsDensity);
constexpr int16_t kContentsTailGap = UiHighDpiProfile::enabled ? 8 : 6;
constexpr uint8_t kContentsButtonRadius = 10;
constexpr uint8_t kContentsButtonBorder = UiHighDpiProfile::enabled ? 2 : 1;
constexpr int16_t kContentsBarW = UiHighDpiProfile::enabled ? 4 : 3;
constexpr int16_t kContentsBarGap = UiHighDpiProfile::enabled ? 10 : 6;
// State badge: a 16 px glyph and its word (FREE / VIP / VIP¥ / LOCK). The
// glyph is a unit mark; the word carries the meaning.
constexpr int16_t kContentsBadgeGlyph = 16;
constexpr int16_t kContentsBadgeGap = 6;
// Bookmarks: the tail is "45%  [x]" — the progress keeps its own column just
// left of the delete button, so it needs a fixed reserve wide enough for 100%.
constexpr int16_t kBookmarkValueW = UiHighDpiProfile::enabled ? 64 : 44;
// Fixed maintenance strip above the Contents list (604 x 88 in the design).
constexpr int16_t kContentsUpdateH = UiHighDpiProfile::enabled ? 88 : 56;
constexpr int16_t kContentsUpdateGap = UiHighDpiProfile::enabled ? 12 : 6;
constexpr int16_t kContentsUpdateIcon = kContentsIcon;
// Contents rows follow the design's 88 px rhythm instead of the shared list's
// denser row height; the badge/action columns are sized from this.
constexpr int16_t kContentsRowH = UiHighDpiProfile::enabled ? 88 : 56;
// Reserve the tail slot with a visible dummy string and then erase it before
// drawing the icons. The list trims/collapses whitespace, so spaces did not
// actually reserve the column: the chapter title kept drawing under the icons
// and showed through their transparent holes as stray dots. 'M' has a real
// advance, valueMaxWidth caps it at the runtime tail width, and the fill below removes
// every dummy pixel (and anything else the list painted in that slot).
constexpr char kTailPlaceholder[] =
    "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";  // 32 real-advance glyphs
constexpr int16_t kPanelTabH = UiHighDpiProfile::enabled ? 64 : 44;
// Type panel (refreshed UI frame 05): a 546 px card, a 48x4 grabber, four tabs and
// a strict 2x4 grid of 96 px cells. Every control is 56 px high and every step
// button 56x56, so the two columns line up on one baseline.
constexpr int16_t kTextSheetH = UiHighDpiProfile::enabled ? 546 : 414;
constexpr int16_t kTextCellH = UiHighDpiProfile::enabled ? 96 : 72;
constexpr int16_t kTextControlH = UiHighDpiProfile::enabled ? 56 : 32;
constexpr int16_t kTextStepW = UiHighDpiProfile::enabled ? 56 : 32;
constexpr int16_t kTextCellPad = UiHighDpiProfile::enabled ? 16 : 8;
constexpr int16_t kTextDoneH = UiHighDpiProfile::enabled ? 60 : 44;
constexpr const char* kContentsBadgeLabels[] = {"FREE", "VIP", "VIP¥", "LOCK"};

// The reader hands the Text rows as "- 12 pt +" / "- 宽松 +" strings (the
// inline stepper affordance its edge taps use). The grid card draws its own
// minus/plus buttons, so keep only the value in the middle.
std::string stepValueText(const char* raw) {
  if (raw == nullptr) return {};
  std::string text(raw);
  if (text.rfind("- ", 0) == 0) text.erase(0, 2);
  static const char* const kSuffixes[] = {" +", " ＋", "+", "＋"};
  for (const char* suffix : kSuffixes) {
    const size_t length = std::strlen(suffix);
    if (text.size() >= length && text.compare(text.size() - length, length, suffix) == 0) {
      text.erase(text.size() - length);
      break;
    }
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

// D11 (design frames 09/10): the focused row's marker -- a solid ink bar at the
// row's left edge, the design's alternative to a full-row inversion (inversion
// stays reserved for current-value semantics). 6x40, BW1: set bit = ink, one
// byte per row, MSB first.
constexpr uint8_t kCpyFocusBarBits[40] = {0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC,
                                          0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC,
                                          0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC,
                                          0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC};
constexpr fui::BitmapRef kCpyFocusBar{kCpyFocusBarBits, 6, 40, fui::BitmapFormat::BW1, true};

// Scrub row: two small round-cornered chapter buttons flanking a thin progress
// track with a round knob -- the reading page's chrome is light, so the
// controls stay slim rather than control-center sized.
constexpr int16_t kScrubButton =
    UiHighDpiProfile::enabled ? UiHighDpiProfile::buttonHeight : 36;  // chapter step buttons (square)
constexpr int16_t kScrubKnob = UiHighDpiProfile::enabled ? 28 : 16;   // round knob on the 2px progress track
constexpr int16_t kScrubGap = 12;                                     // air between the buttons and the track
// Bottom sheet height for the panels. ListNav fits whole rows in the remaining
// list area; any spare pixels stay between the list and the switcher.
constexpr int kPanelHeightPercent = 62;
// Cap the sheet may grow to when rounding the list area up to a whole row.
constexpr int kPanelHeightMaxPercent = 72;
// Landscape has less vertical room; leave a narrow page strip for tap-to-dismiss.
constexpr int kLandscapePanelHeightPercent = 88;
}  // namespace

ReaderToolbarUi::ReaderToolbarUi(GfxRenderer& renderer) : UiAppHost(renderer, true), renderer_(renderer) {}

void ReaderToolbarUi::drawContentsFocusFrame() const {
  if (contentsFocusRect_.width <= 0 || contentsFocusRect_.height <= 0) return;
  renderer_.drawRoundedRect(contentsFocusRect_.x, contentsFocusRect_.y, contentsFocusRect_.width,
                            contentsFocusRect_.height, 2, 6, true);
}

void ReaderToolbarUi::begin() {
  resetUi();
  pending_ = Routed{};
  nav_.reset();
  for (fui::ActionId id = ACTION_DISMISS; id <= ACTION_ROW; ++id) {
    app.on(id, &ReaderToolbarUi::onAction, this);
  }
  app.on(ACTION_STEP_MINUS, &ReaderToolbarUi::onAction, this);
  app.on(ACTION_STEP_PLUS, &ReaderToolbarUi::onAction, this);
  app.setScreen(&ReaderToolbarUi::screenFn, this);
}

void ReaderToolbarUi::render() {
  renderUi();
  // Wrapped rows can fit fewer than the fixed-height estimate; the nav then
  // advances the viewport after layout and asks for a rebuild. Converges (top
  // only moves forward toward the selection); the bound is a backstop.
  for (int pass = 0; pass < 3 && nav_.consumeRebuildNeeded(); ++pass) renderUi();
}

ReaderToolbarUi::Routed ReaderToolbarUi::route(const MappedInputManager& input) {
  pending_ = Routed{};
  // routeHeld: the scrub track is a drag target, so held frames must reach it.
  const auto touch = routeTouch(input, false, /*routeHeld=*/true);
  pending_.routed = touch.routed;
  pending_.x = touch.snap.touchX;
  pending_.y = touch.snap.touchY;
  // Only the release commits a scrub: every held frame dispatches too
  // (dragPermille set), and re-paginating a chapter per frame would be seconds
  // of work per swipe.
  if (pending_.event == Event::Scrub && !touch.snap.touchReleased) pending_.event = Event::None;
  return pending_;
}

void ReaderToolbarUi::onAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  Routed& out = self->pending_;
  out.value = event.value;
  out.permille = event.dragPermille;
  if (event.action == ACTION_STEP_MINUS || event.action == ACTION_STEP_PLUS) {
    // The type grid's stepper buttons: hand the cell index back to the reader,
    // whose stepTextRow() owns the setting ranges and live re-pagination.
    out.event = event.action == ACTION_STEP_MINUS ? Event::TextStepMinus : Event::TextStepPlus;
    out.value = event.value;
  } else if (event.action == ACTION_ROW && self->model_.panel && self->model_.activeTool == kTextTool &&
             (static_cast<int>(event.value) & kTextDoneFlag) != 0) {
    out.value = 0;
    out.event = Event::TextDone;
  } else if (event.action == ACTION_ROW && (static_cast<int>(event.value) & kChipActionFlag) != 0) {
    out.value = static_cast<int16_t>(static_cast<int>(event.value) & ~kChipActionFlag);
    out.event = Event::ChapterAction;
  } else if (event.action == ACTION_ROW && (static_cast<int>(event.value) & kUpdateActionFlag) != 0) {
    out.value = 0;
    out.event = Event::UpdateList;
  } else if (event.action == ACTION_ROW && (static_cast<int>(event.value) & kBookmarkActionFlag) != 0) {
    out.value = 0;
    out.event = Event::BookmarkAction;
  } else if (event.action == ACTION_ROW && (static_cast<int>(event.value) & kBookmarkChipFlag) != 0) {
    out.value = 0;
    out.event = Event::BookmarkChip;
  } else if (event.action == ACTION_ROW && (static_cast<int>(event.value) & kAnnotationChipFlag) != 0) {
    out.value = 0;
    out.event = Event::AnnotationChip;
  } else if (event.action == ACTION_ROW && (static_cast<int>(event.value) & kContentsFilterFlag) != 0) {
    out.value = static_cast<int16_t>(static_cast<int>(event.value) & ~kContentsFilterFlag);
    out.event = Event::ContentsFilter;
  } else if (event.action == ACTION_ROW && (static_cast<int>(event.value) & kExportActionFlag) != 0) {
    out.value = 0;
    out.event = Event::ExportAnnotations;
  } else if (event.action >= ACTION_DISMISS && event.action <= ACTION_ROW) {
    out.event = static_cast<Event>(event.action);
  }
  if (out.event == Event::Scrub && event.dragPermille < 0) out.event = Event::None;
  // A handled action repaints through the reader's own fast path, not through
  // app.invalidate(): the page underneath is the reader's to draw.
  self->app.clearTapFlash();
}

void ReaderToolbarUi::screenFn(UiScreen& screen, void* user) {
  auto* self = static_cast<ReaderToolbarUi*>(user);
  if (self->model_.panel) {
    self->buildPanel(screen);
  } else {
    self->buildToolbar(screen);
  }
}

// The Contents / Text / Bookmarks / More strip. The toolbar draws it idle
// above the scrub row; a panel draws the same strip at its top edge with the
// open tool filled. Keeping one builder (and one geometry) is what makes the
// sheet read as one surface growing upward rather than an icon page swapping
// to an unrelated text page.
void ReaderToolbarUi::buildToolTabs(UiScreen& screen, const fui::Rect row, const int activeTool) {
  const auto& tokens = screen.theme();
  static constexpr StrId kTabLabels[kToolTabs] = {StrId::STR_TOOL_CONTENTS, StrId::STR_TOOL_TYPESETTING,
                                                  StrId::STR_BOOKMARKS, StrId::STR_TOOL_MORE};
  const freeink::Icon* const kIcons[kToolTabs] = {&icon_reader_contents_24, &icon_reader_text_24,
                                                  &icon_reader_bookmark_24, &icon_reader_more_24};
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  const int16_t tabW = static_cast<int16_t>(row.width / kToolTabs);
  const int16_t iconSize = 24;
  const int16_t iconGap = 8;
  for (int i = 0; i < kToolTabs; ++i) {
    const fui::Rect tab{static_cast<int16_t>(row.x + i * tabW), row.y, tabW, row.height};
    const bool active = i == activeTool;
    fui::TextStyle style = tokens.bodyText;
    style.align = fui::TextAlign::Center;
    style.bold = active;
    const char* label = I18N.get(kTabLabels[i]);
    const int16_t textW = screen.target().measureText(style.font, label, style).width;
    const int16_t groupW = static_cast<int16_t>(iconSize + iconGap + textW);
    const int16_t groupX = static_cast<int16_t>(tab.x + (tab.width - groupW) / 2);
    const int16_t iconY = static_cast<int16_t>(tab.y + (tab.height - iconSize) / 2);
    screen.target().bitmap(fui::Rect{groupX, iconY, iconSize, iconSize}, fui::bitmapFromIcon(*kIcons[i]),
                           fui::BitmapMode::Contain, ink);
    screen.target().text(
        fui::Rect{static_cast<int16_t>(groupX + iconSize + iconGap), tab.y,
                  static_cast<int16_t>(textW + 2), tab.height},
        label, style);
    if (active) {
      const int16_t underlineW = 28;
      screen.target().fill(fui::Rect{static_cast<int16_t>(tab.x + (tab.width - underlineW) / 2),
                                     static_cast<int16_t>(tab.bottom() - 4), underlineW, 4},
                           ink, 2);
    }
    screen.frame().hit(tab, ACTION_TOOL, static_cast<int16_t>(i), fui::InputTouch);
  }
  // 2 px rule under the strip (design .tabs border-bottom).
  screen.target().fill(
      fui::Rect{row.x, static_cast<int16_t>(row.bottom() - 2), row.width, 2}, ink);
}

void ReaderToolbarUi::buildToolbar(UiScreen& screen) {
  const auto& tokens = screen.theme();
  const fui::Rect safe = screen.frame().safeRect();

  // Sheet height from its content: tab strip, scrub row, meta line, and the
  // air between them. The tab strip is the same one the expanded panels use,
  // so the two states share their top edge element.
  const int16_t metaH = screen.target().lineHeight(tokens.smallText.font);
  const int16_t contentH = static_cast<int16_t>(tokens.spaceSm + kPanelTabH + tokens.spaceMd + kScrubButton +
                                                tokens.spaceLg + metaH + tokens.spaceSm);
  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;
  // Grabber air matches the frontlight panel's card language (spaceLg around
  // the grabber, spaceMd more toward the free edge) so the two sheets read as
  // the same family.
  sheetProps.grabberMargin = tokens.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(tokens.spaceLg + tokens.spaceMd);
  const int16_t grabberBand =
      static_cast<int16_t>(sheetProps.grabberMargin + sheetProps.grabberHeight + sheetProps.grabberInset);
  const int16_t sheetH = static_cast<int16_t>(contentH + grabberBand);
  sheetTop_ = static_cast<int16_t>(safe.y + safe.height - sheetH);
  screen.sheet(sheetProps, sheetH);
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  // The design's sheet body is the 604 px column at 40 px margins. The device
  // safe rect is 674 px wide at x=5, so inset 35 px per side (not expand): the
  // old negative margin produced a 746 px canvas at x=-31 and clipped the tail.
  const int16_t topMargin = static_cast<int16_t>(sheetTop_ + grabberBand - safe.y);
  screen.setContentMargin(fui::Insets{topMargin, 35, 0, 35});
#endif

  screen.insetContent(fui::Insets{0, tokens.spaceLg, 0, tokens.spaceLg});
  screen.spacer(tokens.spaceSm);

  buildToolTabs(screen, screen.takeTop(kPanelTabH, tokens.spaceMd), model_.activeTool);

  // Scrub row: < [progress track + knob: tap/drag to jump] >
  {
    const fui::Rect band = screen.takeTop(kScrubButton, tokens.spaceLg);
    stepProps_.label = nullptr;
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_back_48);
#else
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_back_24);
#endif
    stepProps_.action = ACTION_PREV;
    stepProps_.inputMask = fui::InputTouch;
    stepProps_.styles.explicitlySet = true;
    stepProps_.styles.normal.background = fui::Paint::solid(fui::Color::White);
    stepProps_.styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.normal.borderWidth = 1;
    stepProps_.styles.normal.radius = tokens.controlRadius;
    stepProps_.styles.selected = stepProps_.styles.normal;
    stepProps_.styles.focused = stepProps_.styles.normal;
    stepProps_.styles.disabled = stepProps_.styles.normal;
    stepProps_.styles.active = stepProps_.styles.normal;
    stepProps_.styles.active.background = fui::Paint::solid(fui::Color::Black);
    stepProps_.styles.active.foreground = fui::Paint::solid(fui::Color::White);
    screen.button(stepProps_, fui::Rect{band.x, band.y, kScrubButton, kScrubButton});
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_next_48);
#else
    stepProps_.icon = fui::bitmapFromIcon(icon_reader_next_24);
#endif
    stepProps_.action = ACTION_NEXT;
    screen.button(stepProps_,
                  fui::Rect{static_cast<int16_t>(band.right() - kScrubButton), band.y, kScrubButton, kScrubButton});

    // Progress: a 2px track with a solid round knob at the book position (the
    // fill edge IS the handle, so nothing else is drawn), the scrub hit rect
    // spanning the whole band height so a finger a little off the line still
    // scrubs. Tap-to-jump / drag arrive as dragPermille along this rect.
    const int16_t trackX = static_cast<int16_t>(band.x + kScrubButton + kScrubGap);
    const int16_t trackW = static_cast<int16_t>(band.width - 2 * (kScrubButton + kScrubGap));
    const int16_t trackY = static_cast<int16_t>(band.y + band.height / 2);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    screen.target().fill(fui::Rect{trackX, static_cast<int16_t>(trackY - 1), trackW, 2}, ink);
    const int permille = std::clamp(model_.progressPermille, 0, 1000);
    const int16_t knobX = static_cast<int16_t>(trackX + (static_cast<int32_t>(trackW - 1) * permille) / 1000);
    screen.target().fill(fui::Rect{static_cast<int16_t>(knobX - kScrubKnob / 2),
                                   static_cast<int16_t>(trackY - kScrubKnob / 2), kScrubKnob, kScrubKnob},
                         ink, static_cast<uint8_t>(kScrubKnob / 2));
    screen.frame().hit(fui::Rect{trackX, band.y, trackW, band.height}, ACTION_SCRUB, 0,
                       fui::InputTouch | fui::InputDrag);
  }

  // Meta line: chapter title (left), chapter page / book percent (right).
  {
    const fui::Rect line = screen.takeTop(metaH, tokens.spaceSm);
    fui::TextStyle titleStyle = tokens.smallText;
    titleStyle.bold = true;
    fui::TextStyle infoStyle = tokens.smallText;
    infoStyle.align = fui::TextAlign::Right;
    const int16_t infoW =
        model_.pageInfo ? screen.target().measureText(infoStyle.font, model_.pageInfo, infoStyle).width : 0;
    const fui::Rect titleRect{line.x, line.y, static_cast<int16_t>(line.width - infoW - tokens.spaceMd), line.height};
    if (model_.chapterTitle) screen.target().text(titleRect, model_.chapterTitle, titleStyle);
    if (model_.pageInfo) screen.target().text(line, model_.pageInfo, infoStyle);
  }
}

void ReaderToolbarUi::buildPanel(UiScreen& screen) {
  contentsFocusRect_ = freeink::ui::Rect{};
  const auto& tokens = screen.theme();
  const fui::Rect safe = screen.frame().safeRect();
  const int16_t side = tokens.spaceLg;
  const bool textPanel = model_.activeTool == kTextTool;
  const bool contents = model_.panel && model_.activeTool == 0;

  // refreshed UI frame 05: 20 px top corners, a 48x4 grabber 9 px below the free
  // edge, and 9 px of air before the tab strip (22 px band in total).
  fui::SheetProps sheetProps;
  sheetProps.anchor = fui::SheetEdge::Bottom;
  sheetProps.dismissAction = ACTION_DISMISS;  // tap the page above the sheet = back to the toolbar
  sheetProps.grabberWidth = 48;
  sheetProps.grabberHeight = 4;
  sheetProps.grabberInset = 9;
  sheetProps.grabberMargin = 9;
  sheetProps.radius = 20;
  const int16_t grabberBand = 22;

  const bool landscape = safe.width > safe.height;
  const int16_t listRowGap =
      model_.denseRows ? tokens.listRowGap : std::max(tokens.listRowGap, tokens.listTouchRowGap);
  const int16_t listRowH =
      contents ? kContentsRowH
      : model_.denseRows ? static_cast<int16_t>(uiThemeMetrics(true).listRowHeight)
                         : tokens.rowHeight;
  const int16_t listStride = static_cast<int16_t>(listRowH + listRowGap);
  // A 56 px button has to fit the actual row height. Short rows shrink the
  // frame and the glyph rather than letting either overhang.
  const int16_t contentsButtonFit =
      std::max<int16_t>(24, std::min<int16_t>(kContentsButton, static_cast<int16_t>(listRowH - 8)));
  const int16_t contentsIconFit =
      std::max<int16_t>(16, std::min<int16_t>(24, static_cast<int16_t>(contentsButtonFit - 16)));

  int16_t sheetH;
  if (textPanel) {
    sheetH = std::min<int16_t>(static_cast<int16_t>(safe.height - 16), kTextSheetH);
  } else {
    const int16_t chipsH = model_.bookmarkChips ? static_cast<int16_t>(kBookmarkChipsH + tokens.spaceSm) : 0;
    const int16_t filterH =
        (contents && model_.contentsFilters) ? static_cast<int16_t>(kBookmarkChipsH + tokens.spaceSm) : 0;
    const int16_t chrome = static_cast<int16_t>(
        grabberBand + kPanelTabH + tokens.spaceMd + tokens.spaceSm + chipsH + filterH +
        ((contents && model_.contentsUpdateAction) || model_.bookmarkAction || model_.annotationExportAction
             ? kContentsUpdateH + tokens.spaceSm
             : 0));
    const int16_t target = static_cast<int16_t>((safe.height * (landscape ? 88 : 62)) / 100);
    const int16_t cap = static_cast<int16_t>((safe.height * (landscape ? 94 : 72)) / 100);
    int sheetRows = (target - chrome + listRowGap) / listStride;
    if (static_cast<int16_t>(chrome + (sheetRows + 1) * listStride - listRowGap) <= cap) ++sheetRows;
    if (model_.itemCount > 0 && sheetRows > model_.itemCount) sheetRows = model_.itemCount;
    if (sheetRows < 1) sheetRows = 1;
    sheetH = static_cast<int16_t>(chrome + sheetRows * listStride - listRowGap);
  }
  sheetTop_ = static_cast<int16_t>(safe.y + safe.height - sheetH);
  screen.sheet(sheetProps, sheetH);
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
  // The design's sheet body is the 604 px column at 40 px margins. The device
  // safe rect is 674 px wide at x=5, so inset 35 px per side (not expand): the
  // old negative margin produced a 746 px canvas at x=-31 and clipped the tail.
  const int16_t topMargin = static_cast<int16_t>(sheetTop_ + grabberBand - safe.y);
  screen.setContentMargin(fui::Insets{topMargin, 35, 0, 35});
#endif


  // Design frames 09/10: the sheet carries its own tab strip. It replaces both
  // the old panel title and the bottom icon switcher, so every tab is visible
  // whichever panel is open.
  buildToolTabs(screen, screen.takeTop(kPanelTabH, textPanel ? 0 : tokens.spaceMd).inset(fui::Insets{0, side, 0, side}),
                model_.activeTool);

  // Design padding for the chrome bands: 40 px each side on the 684 px panel
  // (the 604 px content column), tighter on the low-density boards.
  const int16_t chromePad = UiHighDpiProfile::enabled ? 40 : 12;
  if (contents && model_.contentsFilters) {
    // The filter strip doubles as the state legend (refreshed UI frame 06): plain
    // words in a row, the active one bold with a 4 px rule under it. The three
    // filters the reader actually implements are the three words.
    const fui::Rect band = screen.takeTop(kBookmarkChipsH, tokens.spaceSm).inset(fui::Insets{0, chromePad, 0, chromePad});
    static constexpr StrId kLabels[3] = {StrId::STR_FILTER_ALL, StrId::STR_FILTER_CACHED,
                                         StrId::STR_FILTER_UNCACHED};
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    int16_t x = band.x;
    const int16_t ruleY = static_cast<int16_t>(band.bottom() - 4);
    for (int i = 0; i < 3; ++i) {
      fui::TextStyle style = tokens.bodyText;
      style.bold = model_.contentsFilterIndex == i;
      const char* label = I18N.get(kLabels[i]);
      const int16_t textW = screen.target().measureText(style.font, label, style).width;
      const fui::Rect textRect{x, band.y, textW, band.height};
      screen.target().text(textRect, label, style);
      if (model_.contentsFilterIndex == i) {
        screen.target().fill(fui::Rect{x, ruleY, textW, 4}, ink, 2);
      }
      screen.frame().hit(fui::Rect{static_cast<int16_t>(x - tokens.spaceSm), band.y,
                                   static_cast<int16_t>(textW + tokens.spaceSm * 2), band.height},
                         ACTION_ROW, static_cast<int16_t>(kContentsFilterFlag | i), fui::InputTouch);
      x = static_cast<int16_t>(x + textW + tokens.spaceLg);
    }
  }

  if (model_.bookmarkChips) {
    // Two chips, same capsule language as the rest of the sheet: the active one
    // is inverted so the current list is obvious at a glance, and the counts
    // ride on the labels so an empty view still says how much the other holds.
    const fui::Rect band = screen.takeTop(kBookmarkChipsH, tokens.spaceSm).inset(fui::Insets{0, side, 0, side});
    const int16_t chipGap = tokens.spaceSm;
    const int16_t chipW = static_cast<int16_t>((band.width - chipGap) / 2);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    const fui::Paint paper = fui::Paint::solid(fui::Color::White);
    const auto drawChip = [&](const fui::Rect& rect, const std::string& label, const bool active,
                              const int16_t flag) {
      const uint8_t radius = static_cast<uint8_t>(rect.height / 2);
      screen.target().fill(rect, active ? ink : paper, radius);
      if (!active) screen.target().stroke(rect, ink, kContentsButtonBorder, radius);
      fui::TextStyle style = tokens.bodyText;
      style.bold = true;
      style.align = fui::TextAlign::Center;
      style.color = active ? fui::Color::White : fui::Color::Black;
      screen.target().text(rect, label.c_str(), style);
      screen.frame().hit(rect, ACTION_ROW, flag, fui::InputTouch);
    };
    const std::string bookmarkLabel =
        std::string(tr(STR_BOOKMARKS)) + " (" + std::to_string(model_.bookmarkCount) + ")";
    const std::string annotationLabel =
        std::string(tr(STR_ANNOTATIONS)) + " (" + std::to_string(model_.annotationCount) + ")";
    drawChip(fui::Rect{band.x, band.y, chipW, band.height}, bookmarkLabel, !model_.annotationsChip,
             kBookmarkChipFlag);
    drawChip(fui::Rect{static_cast<int16_t>(band.x + chipW + chipGap), band.y, chipW, band.height}, annotationLabel,
             model_.annotationsChip, kAnnotationChipFlag);
  }

  if (contents && model_.contentsUpdateAction) {
    // Fixed maintenance strip (604 x 88 in the design): a full-width command
    // row above the chapters. It used to be the list's first row, which made a
    // catalogue-wide command look like chapter 0 and let it scroll away.
    const fui::Rect band =
        screen.takeTop(kContentsUpdateH, tokens.spaceSm).inset(fui::Insets{0, chromePad, 0, chromePad});
    fui::TextStyle labelStyle = tokens.bodyText;
    labelStyle.bold = true;
    const char* label = tr(STR_JJWXC_UPDATE_LIST);
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    const freeink::Icon* icon = jjwxc_toc::actionIcon(kContentsDensity, jjwxc_toc::Action::Refresh);
    const int16_t groupX = band.x;
    if (icon != nullptr) {
      const int16_t iconY = static_cast<int16_t>(band.y + (band.height - kContentsUpdateIcon) / 2);
      screen.target().bitmap(
          fui::Rect{groupX, iconY, kContentsUpdateIcon, kContentsUpdateIcon}, fui::bitmapFromIcon(*icon),
          kContentsUpdateIcon == icon->w && kContentsUpdateIcon == icon->h ? fui::BitmapMode::Center
                                                                           : fui::BitmapMode::Contain,
          ink);
    }
    const int16_t textW = screen.target().measureText(labelStyle.font, label, labelStyle).width;
    screen.target().text(
        fui::Rect{static_cast<int16_t>(groupX + kContentsUpdateIcon + kContentsUpdateGap), band.y, textW, band.height},
        label, labelStyle);
    // Hairlines above and below mark the strip as chrome, not a chapter row.
    screen.target().fill(fui::Rect{band.x, band.y, band.width, 1}, ink);
    screen.target().fill(fui::Rect{band.x, static_cast<int16_t>(band.bottom() - 1), band.width, 1}, ink);
    screen.frame().hit(band, ACTION_ROW, kUpdateActionFlag, fui::InputTouch);
  }

  if (model_.bookmarkAction || model_.annotationExportAction) {
    // Same fixed maintenance strip as Contents: a page action that never
    // scrolls away and never looks like a list row. In the annotation view the
    // same strip is the export command.
    const bool exporting = model_.annotationExportAction;
    const fui::Rect band =
        screen.takeTop(kContentsUpdateH, tokens.spaceSm).inset(fui::Insets{0, side, 0, side});
    fui::TextStyle labelStyle = tokens.bodyText;
    labelStyle.bold = true;
    const char* label = exporting ? tr(STR_EXPORT_ANNOTATIONS)
                                  : (model_.bookmarkActionRemove ? tr(STR_REMOVE_BOOKMARK) : tr(STR_ADD_BOOKMARK));
    const freeink::Icon& stripIcon = exporting ? icon_download_32 : icon_bookmark_32;
    const int16_t textW = screen.target().measureText(labelStyle.font, label, labelStyle).width;
    const int16_t groupW = static_cast<int16_t>(kContentsUpdateIcon + kContentsUpdateGap + textW);
    const int16_t groupX = std::max<int16_t>(band.x, static_cast<int16_t>(band.right() - groupW));
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    const int16_t iconY = static_cast<int16_t>(band.y + (band.height - kContentsUpdateIcon) / 2);
    screen.target().bitmap(fui::Rect{groupX, iconY, kContentsUpdateIcon, kContentsUpdateIcon},
                           fui::bitmapFromIcon(stripIcon),
                           kContentsUpdateIcon == stripIcon.w && kContentsUpdateIcon == stripIcon.h
                               ? fui::BitmapMode::Center
                               : fui::BitmapMode::Contain,
                           ink);
    screen.target().text(
        fui::Rect{static_cast<int16_t>(groupX + kContentsUpdateIcon + kContentsUpdateGap), band.y, textW, band.height},
        label, labelStyle);
    screen.target().fill(fui::Rect{band.x, static_cast<int16_t>(band.bottom() - 1), band.width, 1}, ink);
    const int16_t touchX = std::max<int16_t>(band.x, static_cast<int16_t>(groupX - tokens.spaceSm));
    screen.frame().hit(
        fui::Rect{touchX, band.y, static_cast<int16_t>(band.right() - touchX), band.height}, ACTION_ROW,
        exporting ? kExportActionFlag : kBookmarkActionFlag, fui::InputTouch);
  }

  if (textPanel) {
    // refreshed UI frame 05: a strict 2x4 grid. Each cell is a label plus one
    // 56 px control; both columns share one baseline, so nothing staggers.
    const fui::Rect body = screen.body();
    const fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    const fui::Paint paper = fui::Paint::solid(fui::Color::White);
    const int16_t doneH = kTextDoneH;
    const int16_t gridH = static_cast<int16_t>(body.height - doneH - 16);
    const int16_t cellH = std::max<int16_t>(48, std::min<int16_t>(kTextCellH, static_cast<int16_t>(gridH / 4)));
    const int16_t cellW = static_cast<int16_t>(body.width / 2);
    const uint8_t ctrlRadius = 10;

    // Hairlines: one vertical between the columns, one horizontal between the
    // rows, one under the grid -- the design draws the grid, not eight cards.
    for (int row = 0; row <= 4; ++row) {
      screen.target().fill(
          fui::Rect{body.x, static_cast<int16_t>(body.y + row * cellH - (row == 4 ? 1 : 0)), body.width, 1}, ink);
    }
    screen.target().fill(fui::Rect{static_cast<int16_t>(body.x + cellW), body.y, 1, cellH * 4}, ink);

    for (int cell = 0; cell < 8; ++cell) {
      const int column = cell % 2;
      const int row = cell / 2;
      const fui::Rect rect{static_cast<int16_t>(body.x + column * cellW),
                           static_cast<int16_t>(body.y + row * cellH), cellW, cellH};
      const fui::Rect inner{static_cast<int16_t>(rect.x + kTextCellPad), static_cast<int16_t>(rect.y + 8),
                            static_cast<int16_t>(rect.width - kTextCellPad * 2),
                            static_cast<int16_t>(rect.height - 16)};
      fui::TextStyle labelStyle = tokens.smallText;
      labelStyle.bold = true;
      const std::string label = model_.rowText ? model_.rowText(cell) : std::string();
      // The 56 px control owns the bottom of the cell; the label sits directly
      // above it. Clamping the label to that gap is what removed the old
      // half-line collision between "行距" and "段距".
      const int16_t ctrlY = static_cast<int16_t>(rect.y + rect.height - 4 - kTextControlH);
      const int16_t labelH = screen.target().lineHeight(labelStyle.font);
      const int16_t labelY =
          static_cast<int16_t>(std::max<int16_t>(rect.y + 8, static_cast<int16_t>(ctrlY - labelH - 2)));
      screen.target().text(fui::Rect{inner.x, labelY, inner.width, labelH}, label.c_str(), labelStyle);
      const int16_t ctrlW = inner.width;
      const std::string value = model_.rowValue ? model_.rowValue(cell) : std::string();

      const auto drawButton = [&](const fui::Rect& box, const char* text, const fui::TextStyle& style) {
        screen.target().fill(box, paper, ctrlRadius);
        screen.target().stroke(box, ink, 2, ctrlRadius);
        screen.target().text(box, text, style);
      };

      fui::TextStyle valueStyle = tokens.bodyText;
      valueStyle.align = fui::TextAlign::Center;

      if (cell == 0) {
        // Font family: a picker button (Aa + current face + chevron).
        const fui::Rect box{inner.x, ctrlY, ctrlW, kTextControlH};
        screen.target().fill(box, paper, ctrlRadius);
        screen.target().stroke(box, ink, 2, ctrlRadius);
        fui::TextStyle aa = tokens.bodyText;
        aa.bold = true;
        aa.align = fui::TextAlign::Left;
        screen.target().text(fui::Rect{static_cast<int16_t>(box.x + 12), box.y, 40, box.height}, "Aa", aa);
        const int16_t chevronW = 18;
        const int16_t textW = static_cast<int16_t>(box.width - 12 - 40 - 6 - chevronW - 8);
        if (textW > 0) {
          screen.target().text(fui::Rect{static_cast<int16_t>(box.x + 58), box.y, textW, box.height}, value.c_str(),
                               tokens.smallText);
        }
        screen.target().text(
            fui::Rect{static_cast<int16_t>(box.right() - 26), box.y, 18, box.height}, ">", tokens.smallText);
        screen.frame().hit(box, ACTION_ROW, 0, fui::InputTouch);
      } else if (cell >= 1 && cell <= 4) {
        // Stepper: one 56 px rounded box, minus and plus in 56 px end cells.
        const fui::Rect box{inner.x, ctrlY, ctrlW, kTextControlH};
        const int16_t stepW = std::min<int16_t>(kTextStepW, static_cast<int16_t>(ctrlW / 3));
        screen.target().fill(box, paper, ctrlRadius);
        screen.target().stroke(box, ink, 2, ctrlRadius);
        screen.target().fill(fui::Rect{static_cast<int16_t>(box.x + stepW), box.y, 1, box.height}, ink);
        screen.target().fill(fui::Rect{static_cast<int16_t>(box.right() - stepW - 1), box.y, 1, box.height}, ink);
        const fui::Rect minus{box.x, box.y, stepW, box.height};
        const fui::Rect plus{static_cast<int16_t>(box.right() - stepW), box.y, stepW, box.height};
        screen.target().text(minus, "-", valueStyle);
        screen.target().text(plus, "+", valueStyle);
        const std::string shown = stepValueText(value.c_str());
        screen.target().text(
            fui::Rect{static_cast<int16_t>(minus.right()), box.y, static_cast<int16_t>(plus.x - minus.right()),
                      box.height},
            shown.c_str(), valueStyle);
        screen.frame().hit(minus, ACTION_STEP_MINUS, static_cast<int16_t>(cell), fui::InputTouch);
        screen.frame().hit(plus, ACTION_STEP_PLUS, static_cast<int16_t>(cell), fui::InputTouch);
        screen.frame().hit(
            fui::Rect{static_cast<int16_t>(minus.right()), box.y, static_cast<int16_t>(plus.x - minus.right()),
                      box.height},
            ACTION_ROW, static_cast<int16_t>(cell), fui::InputTouch);
      } else if (cell == 5 || cell == 6) {
        // Switch: 72x40 (design) centred in the 56 px control band.
        const int16_t trackW = UiHighDpiProfile::enabled ? 72 : 56;
        const int16_t trackH = UiHighDpiProfile::enabled ? 40 : 32;
        const int16_t trackY = static_cast<int16_t>(ctrlY + (kTextControlH - trackH) / 2);
        fui::TextStyle stateStyle = tokens.bodyText;
        screen.target().text(fui::Rect{inner.x, ctrlY, static_cast<int16_t>(trackW + 6), kTextControlH},
                             value.c_str(), stateStyle);
        const bool on = value == tr(STR_YES);
        const fui::Rect track{static_cast<int16_t>(inner.x + trackW + 6), trackY, trackW, trackH};
        if (on) {
          screen.target().fill(track, ink, static_cast<uint8_t>(trackH / 2));
        } else {
          screen.target().stroke(track, ink, 2, static_cast<uint8_t>(trackH / 2));
        }
        const int16_t knob = static_cast<int16_t>(trackH - 8);
        const int16_t knobX = on ? static_cast<int16_t>(track.right() - 4 - knob) : static_cast<int16_t>(track.x + 4);
        screen.target().fill(fui::Rect{knobX, static_cast<int16_t>(track.y + 4), knob, knob},
                             on ? paper : ink, static_cast<uint8_t>(knob / 2));
        screen.frame().hit(track, ACTION_ROW, static_cast<int16_t>(cell), fui::InputTouch);
      } else {
        // Restore defaults: the same outlined button language as the pickers.
        fui::TextStyle resetStyle = tokens.bodyText;
        resetStyle.align = fui::TextAlign::Center;
        resetStyle.bold = true;
        drawButton(fui::Rect{inner.x, ctrlY, ctrlW, kTextControlH}, tr(STR_DEFAULT_VALUE), resetStyle);
        screen.frame().hit(fui::Rect{inner.x, ctrlY, ctrlW, kTextControlH}, ACTION_ROW,
                           static_cast<int16_t>(cell), fui::InputTouch);
      }
    }

    // Done: 604 x 60 black pill at the card's foot (design .rp .done).
    const fui::Rect done{static_cast<int16_t>(body.x + 40), static_cast<int16_t>(sheetTop_ + sheetH - 8 - doneH),
                         static_cast<int16_t>(body.width - 80), doneH};
    screen.target().fill(done, ink, static_cast<uint8_t>(doneH / 2));
    fui::TextStyle doneStyle = tokens.bodyText;
    doneStyle.align = fui::TextAlign::Center;
    doneStyle.bold = true;
    doneStyle.color = fui::Color::White;
    screen.target().text(done, tr(STR_DONE), doneStyle);
    screen.frame().hit(done, ACTION_DISMISS, 0, fui::InputTouch);
    return;
  }

  // Contents tail (frame 06): a 16 px state glyph + its word, then one 56x56
  // rounded action button. Size the slot from the widest word so the column
  // never shifts between FREE, VIP, VIP¥ and LOCK rows.
  int16_t badgeTextW = 0;
  for (const char* label : kContentsBadgeLabels) {
    badgeTextW = std::max<int16_t>(badgeTextW,
                                   screen.target().measureText(tokens.smallText.font, label, tokens.smallText).width);
  }
  const int16_t badgeGroupW = static_cast<int16_t>(kContentsBadgeGlyph + kContentsBadgeGap + badgeTextW);
  const int16_t tailW =
      model_.rowTail ? static_cast<int16_t>(contents ? badgeGroupW + 12 + contentsButtonFit + 4
                                                     : contentsButtonFit + kContentsTailGap + kBookmarkValueW)
                     : 0;
  int16_t rowInset = tokens.spaceLg;
  if (model_.currentChapterIndex >= 0) {
    // The current-chapter bar lives in this margin, clear of the title. The old
    // selection bitmap was drawn inside the text area and collided with it.
    rowInset = std::max<int16_t>(rowInset, kContentsBarW + kContentsBarGap + 4);
  }
  listProps_.count = static_cast<uint16_t>(std::max(0, model_.itemCount));
  listProps_.action = ACTION_ROW;
  listProps_.inputMask = fui::InputTouch;  // physical buttons stay with the reader
  listProps_.rowHeight = listRowH;
  listProps_.rowGap = listRowGap;
  listProps_.toggleCheckbox = false;
  listProps_.toggleWidth = UiHighDpiProfile::enabled ? 48 : 28;
  listProps_.toggleHeight = UiHighDpiProfile::enabled ? 28 : 18;
  listProps_.toggleRadius = UiHighDpiProfile::enabled ? 14 : 9;
  listProps_.toggleKnobRadius = UiHighDpiProfile::enabled ? 10 : 6;
  listProps_.toggleKnobInset = 3;
  listProps_.toggleBorderWidth = 2;
  listProps_.labelText = tokens.bodyText;
  listProps_.sidePadding = 0;
  listProps_.rowInset = rowInset;
  listProps_.valueInset = 0;
  listProps_.valueMaxWidth = tailW > 0 ? tailW : -1;
  if (UITheme::getInstance().usesCpyLayout()) {
    fui::StyleSet rowStyles = fui::defaultListRowStyles();
    // Never invert a row: the cursor is a 2 px inner frame (Contents) or the
    // thin focus bar (bookmarks), so the chapter text and its badges keep
    // their normal ink.
    rowStyles.selected = rowStyles.normal;
    if (contents && model_.fullRowSelection) {
      listProps_.selectionMarker = fui::SelectionMarker::None;
    } else {
      listProps_.selectionMarker = fui::SelectionMarker::Bitmap;
      listProps_.markerBitmap = kCpyFocusBar;
      listProps_.markerInset = 6;
    }
    listProps_.rowStyles = rowStyles;
  }
  const fui::Rect listRect = screen.body();
  const int count = std::max(0, model_.itemCount);
  nav_.selected = std::clamp(model_.selectedIndex, -1, count - 1);
  nav_.followOnBuild = nav_.selected >= 0;
  nav_.followPending = false;
  nav_.syncToProps(listRect, listProps_.rowHeight, listRowGap, count, listProps_);

  const int windowCount = std::min({nav_.visibleRows, count - nav_.top, kMaxWindow});
  for (int i = 0; i < windowCount; ++i) {
    const int index = nav_.top + i;
    windowLabels_[i] = model_.rowText ? model_.rowText(index) : std::string();
    const RowTail tail = model_.rowTail ? model_.rowTail(index) : RowTail{};
    // The tail slot has to be reserved in the list's value column or a long
    // title would run underneath the chip. Full-width spaces are invisible and
    // give the list an exact width to subtract.
    windowValues_[i] = tail.badge != TailBadge::None
                           ? std::string(kTailPlaceholder)
                           : (tail.action == TailAction::Delete
                                  ? std::string()
                                  : (model_.rowValue ? model_.rowValue(index) : ""));
    fui::ListItem item;
    item.label = windowLabels_[i].c_str();
    item.value = windowValues_[i].empty() ? nullptr : windowValues_[i].c_str();
    if (model_.rowCheckbox) model_.rowCheckbox(model_.rowCheckboxContext, index, item);
    item.actionValue = static_cast<int16_t>(index);
    windowItems_[i] = item;
  }
  listProps_.items = windowItems_;
  listProps_.itemsWindowFirst = static_cast<uint16_t>(nav_.top);
  listProps_.itemsWindowCount = static_cast<uint16_t>(std::max(0, windowCount));
  listProps_.valueText = tokens.bodyText;
  listProps_.valueText.bold = true;
  if (count > 0) {
    screen.list(listProps_);
  }

  if (!contents && !model_.rowTail && model_.currentChapterIndex < 0) return;

  // The list paints rows and their interaction rectangles; these decorations
  // are laid on top in the same frame. Each glyph gets its own rounded square,
  // the same 64 px frame + 16 px radius as the library's sync button, so the
  // touch target is the button rather than the artwork. Both frames are
  // geometrically centered in the row: no optical shift, which is what made
  // the download glyph read higher than FREE.
  const int16_t rowH = listProps_.rowHeight;
  // Decoration columns use the panel's full content width (604 px at 40 px
  // margins), not the list's title column: the current-row frame and the tail
  // buttons belong to the whole row, including the badge slot.
  // The 684 px device width is the authority for the row's full extent: the
  // FreeInkUI list is handed a narrower title column, and its safe rect is
  // inset again by the sheet. The design's content column is 604 px at 40 px
  // margins, and the current-row frame + tail belong to that whole column.
  constexpr int16_t kRowMargin = 40;
  // ReadPico's design column is 604 px at 40 px margins. The sheet's content
  // canvas reports a narrower width through FreeInkUI, but the target still
  // accepts the full device width; use the renderer-provided width for the
  // row frame and tail.
  const int16_t panelWidth = UiHighDpiProfile::enabled ? 684
                                                       : (model_.screenWidth > 0 ? model_.screenWidth : 532);
  // The list leaves its own clip active; reset it or every decoration beyond
  // the title column (frame, badge, action button) is cut at the list edge.
  screen.target().setClipRect(fui::Rect{0, 0, panelWidth, static_cast<int16_t>(screen.frame().device().height)});
  const int16_t fullRowX = kRowMargin;
  const int16_t fullRowRight = static_cast<int16_t>(panelWidth - kRowMargin);
  const int16_t fullRowW = static_cast<int16_t>(fullRowRight - fullRowX);
  const int16_t rowAreaX = fullRowX;
  const int16_t rowAreaW = fullRowW;
  const int16_t tailStart =
      std::min<int16_t>(static_cast<int16_t>(fullRowRight - tailW),
                        static_cast<int16_t>(listRect.x + listRect.width - tailW));
  const int16_t buttonSize = contentsButtonFit;
  const int16_t iconSize = contentsIconFit;
  const int16_t gap = kContentsTailGap;
  const uint8_t buttonRadius =
      static_cast<uint8_t>(std::min<int16_t>(kContentsButtonRadius, static_cast<int16_t>(buttonSize / 4)));
  for (int i = 0; i < windowCount; ++i) {
    const int index = nav_.top + i;
    const int16_t rowY = static_cast<int16_t>(listRect.y + i * (rowH + listRowGap));
    const bool selected = index == nav_.selected;
    if (index == model_.currentChapterIndex) {
      const int16_t barX = static_cast<int16_t>(rowAreaX - kContentsBarGap - kContentsBarW);
      if (barX >= listRect.x) {
        screen.target().fill(fui::Rect{barX, static_cast<int16_t>(rowY + 4), kContentsBarW,
                                       static_cast<int16_t>(rowH - 8)},
                             fui::Paint::solid(fui::Color::Black));
      }
    }
    const bool framed = contents && rowH > 8 && listRect.width > 8 &&
                        (selected || (model_.selectedIndex < 0 && index == model_.currentChapterIndex));
    if (framed) {
      // Cursor row (button boards) or, on touch boards, the chapter being read:
      // 2 px inner frame (frame 06 .crow.focus), never a full-row inversion.
      // The current-chapter bar stays put when the cursor moves away.
      // Draw the full-row frame straight through the renderer: the FreeInkUI
      // list leaves a clip at its title column, and a target-side stroke was
      // cut at x≈500 even though the row is 604 px wide.
      contentsFocusRect_ = freeink::ui::Rect{static_cast<int16_t>(rowAreaX + 2), static_cast<int16_t>(rowY + 2),
                                static_cast<int16_t>(rowAreaW - 4), static_cast<int16_t>(rowH - 4)};
      renderer_.drawRoundedRect(contentsFocusRect_.x, contentsFocusRect_.y, contentsFocusRect_.width,
                                contentsFocusRect_.height, 2, 6, true);
      // JJWXC rows carry "87. " as a display-only ordinal; the focused row
      // inverts just that prefix (frame 06 .crow.focus .nu). The list draws
      // the label at rowAreaX with the band vertically centred, so a black
      // chip plus a white redraw lands exactly over it.
      // Only a button-driven cursor inverts its ordinal. On touch the list has
      // no cursor, and the white redraw over the current row left a "1." ghost
      // because the list's label font/inset differ from this overlay.
      const std::string& labelText = windowLabels_[i];
      size_t prefixLen = 0;
      if (model_.selectedIndex < 0) prefixLen = labelText.size();
      while (prefixLen < labelText.size() && labelText[prefixLen] >= '0' && labelText[prefixLen] <= '9') ++prefixLen;
      if (prefixLen > 0 && prefixLen + 1 < labelText.size() && labelText[prefixLen] == '.' &&
          labelText[prefixLen + 1] == ' ') {
        prefixLen += 2;
        const std::string prefix = labelText.substr(0, prefixLen);
        const int16_t lineH = screen.target().lineHeight(tokens.bodyText.font);
        const int16_t textW =
            screen.target().measureText(tokens.bodyText.font, prefix.c_str(), tokens.bodyText).width;
        const int16_t textY = static_cast<int16_t>(rowY + (rowH - lineH) / 2);
        const int16_t prefixX = rowAreaX;
        screen.target().fill(fui::Rect{prefixX, textY, static_cast<int16_t>(textW + 2), lineH},
                             fui::Paint::solid(fui::Color::Black), 2);
        fui::TextStyle prefixStyle = tokens.bodyText;
        prefixStyle.color = fui::Color::White;
        screen.target().text(fui::Rect{prefixX, textY, static_cast<int16_t>(textW + 2), lineH}, prefix.c_str(),
                             prefixStyle);
      }
    }
    if (!model_.rowTail) continue;
    const RowTail tail = model_.rowTail(index);
    if (tail.action == TailAction::Delete) {
      // Bookmarks: one rounded button in the tail, exactly the library sync
      // button frame. The glyph is a multiplication sign so it needs no new
      // asset and reads the same in every language.
      const int16_t slotX = static_cast<int16_t>(rowAreaX + rowAreaW - buttonSize);
      const fui::BoxStyle& rowStyle =
          listProps_.rowStyles.resolve(selected ? fui::StateSelected : fui::StateNormal);
      const fui::Paint bg = rowStyle.background.kind == fui::PaintKind::None
                                ? fui::Paint::solid(selected ? fui::Color::Black : fui::Color::White)
                                : rowStyle.background;
      const fui::Paint ink = rowStyle.foreground.kind == fui::PaintKind::None
                                 ? fui::Paint::solid(selected ? fui::Color::White : fui::Color::Black)
                                 : rowStyle.foreground;
      screen.target().fill(fui::Rect{slotX, rowY, buttonSize, rowH}, bg, rowStyle.radius);
      // Progress keeps the column just left of the delete button. The list no
      // longer draws this row's value (it would sit under the button), so the
      // tail owns both the text and the button.
      const std::string progress = model_.rowValue ? model_.rowValue(index) : std::string();
      if (!progress.empty()) {
        fui::TextStyle progressStyle = tokens.bodyText;
        progressStyle.bold = true;
        progressStyle.align = fui::TextAlign::Right;
        progressStyle.color = selected ? fui::Color::White : fui::Color::Black;
        screen.target().text(
            fui::Rect{static_cast<int16_t>(slotX - gap - kBookmarkValueW), rowY, kBookmarkValueW, rowH},
            progress.c_str(), progressStyle);
      }
      const int16_t buttonY = static_cast<int16_t>(rowY + (rowH - buttonSize) / 2);
      screen.target().stroke(fui::Rect{slotX, buttonY, buttonSize, buttonSize}, ink, kContentsButtonBorder,
                             buttonRadius);
      fui::TextStyle xStyle = tokens.bodyText;
      xStyle.bold = true;
      xStyle.align = fui::TextAlign::Center;
      screen.target().text(fui::Rect{slotX, buttonY, buttonSize, buttonSize}, "×", xStyle);
      screen.frame().hit(fui::Rect{slotX, rowY, buttonSize, rowH}, ACTION_ROW,
                         static_cast<int16_t>(kChipActionFlag | index), fui::InputTouch);
      continue;
    }
    if (tail.badge == TailBadge::None) continue;

    jjwxc_toc::Badge badgeKind = jjwxc_toc::Badge::Free;
    switch (tail.badge) {
      case TailBadge::Free: badgeKind = jjwxc_toc::Badge::Free; break;
      case TailBadge::Vip: badgeKind = jjwxc_toc::Badge::Vip; break;
      case TailBadge::VipYen: badgeKind = jjwxc_toc::Badge::VipYen; break;
      case TailBadge::Lock: badgeKind = jjwxc_toc::Badge::Lock; break;
      case TailBadge::None: continue;
    }
    const jjwxc_toc::Action actionKind =
        tail.action == TailAction::Download ? jjwxc_toc::Action::Download
        : tail.action == TailAction::Refresh ? jjwxc_toc::Action::Refresh
                                             : jjwxc_toc::Action::None;
    // State column: a 16 px glyph and its word, left-aligned in the reserved
    // slot; the action button owns the far right edge.
    // Badge-only rows (VIP¥) put the state against the right edge; rows with
    // an action leave the 56 px button slot at that edge.
    const int16_t slotX =
        tail.action == TailAction::None ? static_cast<int16_t>(fullRowRight - badgeGroupW)
                                        : static_cast<int16_t>(fullRowRight - tailW);

    // Erase the dummy reservation and anything the list drew under the icons,    // then paint the badge with the row's own foreground. Without this fill the
    // chapter text (or the old whitespace placeholder) showed through the
    // transparent parts of the glyphs as stray dots.
    const fui::BoxStyle& rowStyle = listProps_.rowStyles.resolve(fui::StateNormal);
    const fui::Paint bg = rowStyle.background.kind == fui::PaintKind::None ? fui::Paint::solid(fui::Color::White)
                                                                           : rowStyle.background;
    const fui::Paint ink = rowStyle.foreground.kind == fui::PaintKind::None
                               ? fui::Paint::solid(fui::Color::Black)
                               : rowStyle.foreground;
    // Clear the whole right-hand strip, not just tailW: the list's dummy
    // value is measured against the row value column and its ellipsis could
    // still peek out to the right of a narrower tail.
    // Clear to the device edge: the list's dummy value column can extend past
    // the design's 604 px column (and was the source of the "M..." tail).
    const int16_t slotRight = panelWidth;
    const int16_t clearX = std::min<int16_t>(tailStart, slotX);
    screen.target().fill(fui::Rect{clearX, rowY, static_cast<int16_t>(slotRight - clearX), rowH}, bg,
                         rowStyle.radius);

    const int16_t glyphX = static_cast<int16_t>(slotX + 2);
    const int16_t glyphY = static_cast<int16_t>(rowY + (rowH - kContentsBadgeGlyph) / 2);
    const char* badgeLabel =
        kContentsBadgeLabels[static_cast<int>(tail.badge) - static_cast<int>(TailBadge::Free)];
    int16_t labelX = static_cast<int16_t>(slotX + 2);
    // The badge assets for Free/Vip already spell their word. Painting the
    // glyph and the label together produced "VIP VIP¥"; only the lock glyph
    // is drawn beside its word.
    if (badgeKind == jjwxc_toc::Badge::Lock) {
      const freeink::Icon& badge = jjwxc_toc::badgeIcon(kContentsDensity, badgeKind);
      screen.target().bitmap(fui::Rect{glyphX, glyphY, kContentsBadgeGlyph, kContentsBadgeGlyph},
                             fui::bitmapFromIcon(badge), fui::BitmapMode::Contain, ink);
      labelX = static_cast<int16_t>(glyphX + kContentsBadgeGlyph + kContentsBadgeGap);
    }
    fui::TextStyle badgeStyle = tokens.smallText;
    badgeStyle.color = fui::Color::Black;
    screen.target().text(fui::Rect{labelX, rowY, static_cast<int16_t>(slotRight - labelX), rowH},
                         badgeLabel, badgeStyle);

    const int16_t actionX = static_cast<int16_t>(fullRowRight - buttonSize);
    if (actionKind != jjwxc_toc::Action::None) {
      const freeink::Icon* action = jjwxc_toc::actionIcon(kContentsDensity, actionKind);
      if (action != nullptr) {
        const int16_t buttonY = static_cast<int16_t>(rowY + (rowH - buttonSize) / 2);
        screen.target().stroke(fui::Rect{actionX, buttonY, buttonSize, buttonSize}, ink, kContentsButtonBorder,
                               buttonRadius);
        const fui::Rect iconRect{static_cast<int16_t>(actionX + (buttonSize - iconSize) / 2),
                                 static_cast<int16_t>(buttonY + (buttonSize - iconSize) / 2), iconSize, iconSize};
        screen.target().bitmap(iconRect, fui::bitmapFromIcon(*action),
                               iconSize == action->w && iconSize == action->h ? fui::BitmapMode::Center
                                                                              : fui::BitmapMode::Contain,
                               ink);
      }
    }
    if (tail.badge != TailBadge::Lock) {
      // The badge is state only; the action button answers taps. A badge-only
      // row (VIP¥ = purchase entry) still owns the whole slot.
      if (actionKind != jjwxc_toc::Action::None) {
        screen.frame().hit(fui::Rect{actionX, rowY, buttonSize, rowH}, ACTION_ROW,
                           static_cast<int16_t>(kChipActionFlag | index), fui::InputTouch);
      } else {
        screen.frame().hit(fui::Rect{slotX, rowY, tailW, rowH}, ACTION_ROW,
                           static_cast<int16_t>(kChipActionFlag | index), fui::InputTouch);
      }
    }
  }
}
