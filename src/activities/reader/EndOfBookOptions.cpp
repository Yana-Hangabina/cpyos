#include "EndOfBookOptions.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
// ReaderUtils.h pulls in ActivityManager.h, which only forward-declares Activity while holding
// std::unique_ptr<Activity> members. Destroying that unique_ptr needs the complete type, so the
// definition must be visible here.
#include "activities/Activity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// Suggestion-row thumbnail source height: the shelf-grid size, which the
// converter demonstrably handles (a 96px request comes back as a header-only
// stub, so the 72x96 cell crops DOWN from 254 instead of scaling up).
constexpr int kEobThumbSourceHeight = 254;

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}
}  // namespace

EndOfBookOptions::EndOfBookOptions(GfxRenderer& renderer) : UiAppHost(renderer), renderer(renderer) {}

void EndOfBookOptions::loadOnce(const std::string& currentBookPath) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  {
    const size_t slash = currentBookPath.rfind('/');
    bookTitle = displayName(slash == std::string::npos ? currentBookPath : currentBookPath.substr(slash + 1));
  }
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector.store(0, std::memory_order_relaxed);
  if (!names.empty()) {
    // One-time app setup on the render task, before the first render/route.
    resetUi();
    app.on(ACTION_ROW, &EndOfBookOptions::onRowEvent, this);
    app.setScreen(&EndOfBookOptions::listScreen, this);
    buildRowItems();
    // Warm the suggestion thumbnails now, while the reader holds the frame
    // cleared for us: the framebuffer can be lent as conversion scratch and
    // restored in place, and the paint pass below only ever READS settled
    // files (generating mid-paint fails -- the converter needs that scratch).
    if (UITheme::getInstance().usesCpyLayout()) {
      GfxRenderer::FrameBufferLoan loan(renderer);
      for (size_t i = 0; i < names.size(); ++i) BookCoverLoader::ensureThumbnail(fullPath(i), kEobThumbSourceHeight);
    }
  }
  // Release-publish so the main task, which gates all access on isLoaded, never
  // observes a partially built list (rowItems/rowLabels included)
  isLoaded.store(true, std::memory_order_release);
}

// Populates rowLabels/rowItems from names + the trailing "Home" row. Called
// once here since names never changes after loadOnce() completes.
void EndOfBookOptions::buildRowItems() {
  rowCount = 0;
  for (const auto& name : names) {
    if (rowCount >= MAX_ROWS) break;
    rowLabels[rowCount] = displayName(name);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
  if (rowCount < MAX_ROWS) {
    rowLabels[rowCount] = tr(STR_EOB_HOME);
    fui::ListItem item;
    item.label = rowLabels[rowCount].c_str();
    item.actionValue = static_cast<int16_t>(rowCount);
    rowItems[rowCount] = item;
    rowCount++;
  }
}

bool EndOfBookOptions::menuActive() const { return isLoaded.load(std::memory_order_acquire) && !names.empty(); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

void EndOfBookOptions::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EndOfBookOptions*>(user);
  if (event.value < 0 || event.value > static_cast<int16_t>(self->names.size())) return;
  self->selector.store(event.value, std::memory_order_relaxed);
  // The tapped row leaves this screen (open book or home); a lingering flash
  // would gray an unrelated element on the next render.
  self->app.clearTapFlash();
  self->tappedRow = event.value;
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent record the tapped row.
  tappedRow = -1;
  const auto route = routeTouch(input);
  // cppcheck can't see that route() dispatches into onRowEvent (registered via
  // app.on(ACTION_ROW, ...)), which sets tappedRow, so it flags this as always false.
  // cppcheck-suppress knownConditionTrueFalse
  if (route && tappedRow >= 0) {
    if (tappedRow < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(tappedRow);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" row tapped
  }
  if (route.routed && app.invalidated()) {
    return Action::Redraw;
  }

  const int selectedIndex = selector.load(std::memory_order_relaxed);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(selectedIndex);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" entry selected
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser). Home is reached through the list's Home entry.
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Selection movement on the standard list navigation buttons (side Up/Down plus front
  // Left/Right, orientation swap included). It follows the reader's page-turn semantics
  // (press-triggered by default, release-triggered when a long-press behavior is
  // configured, same rule as ReaderUtils::detectPageTurn). This matters on entry: with
  // press-triggered turns, the press that turned the final page already fired in the
  // reader, and its release must not double-fire into this menu.
  const bool usePress = SETTINGS.longPressButtonBehavior == CrossPointSettings::OFF;
  const auto triggered = [&](const MappedInputManager::Button button) {
    return usePress ? input.wasPressed(button) : input.wasReleased(button);
  };
  const int itemCount = static_cast<int>(names.size()) + 1;  // + "Home" entry
  if (triggered(MappedInputManager::Button::NavPrevious)) {
    selector.store(ButtonNavigator::previousIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  if (triggered(MappedInputManager::Button::NavNext)) {
    selector.store(ButtonNavigator::nextIndex(selectedIndex, itemCount), std::memory_order_relaxed);
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::listScreen(UiScreen& screen, void* user) {
  static_cast<EndOfBookOptions*>(user)->buildListScreen(screen);
}

// CPY celebration header (design frame 32): big serif "the book is done",
// the finished book's name, a double hairline, then the suggestion section
// label. Records where the rows start for buildCpyRows(), which paints in
// the same renderUi() pass right after.
void EndOfBookOptions::drawCpyHeader() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int screenWidth = renderer.getScreenWidth();
  constexpr int kSide = 32;

  int y = safe.y + safe.height / 10;
  UITheme::drawCenteredText(renderer, safe, NOTOSERIF_18_FONT_ID, y, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(NOTOSERIF_18_FONT_ID) + metrics.verticalSpacing;
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, y, bookTitle.c_str(), true);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 24;
  renderer.drawLine(kSide, y, screenWidth - 1 - kSide, y, true);
  renderer.drawLine(kSide, y + 4, screenWidth - 1 - kSide, y + 4, true);
  y += 4 + 24;
  renderer.drawText(UI_12_FONT_ID, kSide, y, tr(STR_EOB_CONTINUE_WITH), true, EpdFontFamily::BOLD);
  cpyRowsTop = y + renderer.getLineHeight(UI_12_FONT_ID) + 12;
}

void EndOfBookOptions::buildCpyRows(UiScreen& screen) {
  const int screenWidth = renderer.getScreenWidth();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  constexpr int kSide = 32;
  constexpr int kRowHeight = 130;  // design .lrow
  constexpr int kThumbWidth = 72;
  constexpr int kThumbHeight = 96;
  constexpr int kPillHeight = 52;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int selected = selector.load(std::memory_order_relaxed);

  int y = cpyRowsTop;
  for (size_t i = 0; i < names.size(); ++i) {
    const bool selectedRow = static_cast<int>(i) == selected;
    if (i > 0) renderer.drawLine(0, y, screenWidth - 1, y, true);

    // Cover thumbnail; a framed placeholder until the cache has one.
    const Rect thumb{kSide, y + (kRowHeight - kThumbHeight) / 2, kThumbWidth, kThumbHeight};
    bool drewCover = false;
    const std::string thumbPath = BookCoverLoader::ensureThumbnail(fullPath(i), kEobThumbSourceHeight);
    if (!thumbPath.empty()) {
      HalFile file;
      if (Storage.openFileForRead("EOB", thumbPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
          drewCover = renderer.drawBitmapCropToFill(bitmap, thumb.x, thumb.y, thumb.width, thumb.height);
        }
      }
    }
    if (!drewCover) renderer.drawRect(thumb.x, thumb.y, thumb.width, thumb.height, 1, true);

    const int pillWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_OPEN)) + 48;
    const int titleX = kSide + kThumbWidth + 24;
    const int titleWidth = std::max(1, screenWidth - kSide - pillWidth - 24 - titleX);
    const std::string shown =
        renderer.truncatedText(UI_12_FONT_ID, rowLabels[i].c_str(), titleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, titleX, y + (kRowHeight - titleLineHeight) / 2, shown.c_str(), true,
                      EpdFontFamily::BOLD);

    const Rect pill{screenWidth - kSide - pillWidth, y + (kRowHeight - kPillHeight) / 2, pillWidth, kPillHeight};
    renderer.fillRoundedRect(pill.x, pill.y, pill.width, pill.height, kPillHeight / 2, Color::White);
    renderer.drawRoundedRect(pill.x, pill.y, pill.width, pill.height, 1, kPillHeight / 2, true);
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_OPEN));
    renderer.drawText(UI_10_FONT_ID, pill.x + (pill.width - labelWidth) / 2,
                      pill.y + (pill.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, tr(STR_OPEN));

    // Selection is a ring, not an inverted band: covers are ink-only bitmap
    // draws, and black ink on an inverted row disappears (the shelf grid uses
    // the same ring so the two shelves agree).
    if (selectedRow) {
      renderer.drawRoundedRect(4, y + 3, screenWidth - 8, kRowHeight - 6, 3, 12, true);
    }

    screen.frame().hit(fui::Rect{0, static_cast<int16_t>(y), static_cast<int16_t>(screenWidth), kRowHeight},
                       ACTION_ROW, static_cast<int16_t>(i), fui::InputTouch);
    y += kRowHeight;
  }

  // Home primary pill (the design's 返回首页), clamped above the hint band so
  // it stays reachable in the reader's rotated orientations.
  y += 26;
  const bool hintsVisible = !gpio.hasTouch() && SETTINGS.showButtonHints;
  const int hintBand = hintsVisible ? UITheme::getInstance().getMetrics().buttonHintsHeight : 0;
  const int pillMaxY = safe.y + safe.height - hintBand - 56 - 8;
  if (y > pillMaxY) y = std::max(cpyRowsTop, pillMaxY);
  const int homeWidth = std::min(400, screenWidth - 2 * kSide);
  const Rect home{(screenWidth - homeWidth) / 2, y, homeWidth, 56};
  const bool homeSelected = selected == static_cast<int>(names.size());
  renderer.fillRoundedRect(home.x, home.y, home.width, home.height, home.height / 2, Color::Black);
  const int homeLabelWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_EOB_HOME));
  renderer.drawText(UI_10_FONT_ID, home.x + (home.width - homeLabelWidth) / 2,
                    home.y + (home.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2, tr(STR_EOB_HOME), false);
  if (homeSelected) {
    renderer.drawRoundedRect(home.x + 4, home.y + 4, home.width - 8, home.height - 8, 2, home.height / 2 - 4, false);
  }
  screen.frame().hit(
      fui::Rect{static_cast<int16_t>(home.x), static_cast<int16_t>(home.y), static_cast<int16_t>(home.width), 56},
      ACTION_ROW, static_cast<int16_t>(names.size()), fui::InputTouch);
}

void EndOfBookOptions::buildListScreen(UiScreen& screen) {
  if (UITheme::getInstance().usesCpyLayout()) {
    buildCpyRows(screen);
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Same layout math as render(): the list band starts under the title/subtitle it
  // draws, and stops above the button hints (the safe-area bottom edge).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int titleY = safe.y + safe.height / 8;
  const int subtitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  const int listTop = subtitleY + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(listTop), static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing),
      static_cast<int16_t>(safe.x)});

  // rowLabels/rowItems were built once in loadOnce() (see buildRowItems())
  // and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.selectedIndex = static_cast<int16_t>(selector.load(std::memory_order_relaxed));
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleMenuInput()
  screen.list(props);
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (!menuActive()) {
    // No suggestions: the historical plain end screen. 3/8 of the screen height matches
    // the previous fixed position on the 480x800 panel and scales to other resolutions.
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    return;
  }

  if (UITheme::getInstance().usesCpyLayout()) {
    // Design frame 32: the celebration header paints first; the rows and the
    // Home pill come through renderUi() below (buildCpyRows), then the hints.
    drawCpyHeader();
    renderUi();
    const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  // Suggestion menu: title, list (+ Home entry) and button hints. The hints are drawn at
  // the physical front buttons, which is a logical side/top edge in the rotated
  // orientations — lay out inside the safe area so nothing hides behind them. Vertical
  // positions derive from the safe-area height and font line heights so other panel
  // resolutions scale (review request on #2532).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int titleY = safe.y + safe.height / 8;
  const int subtitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, titleY, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, subtitleY, tr(STR_EOB_CONTINUE_WITH));

  // The list renders through the FreeInkApp so its rows register touch hit
  // rects; renderUi re-derives the device context, picking up any rotation
  // since construction (reader menu rotate).
  renderUi();

  const auto labels = input.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
