#include "AppVisibilitySettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "AppCatalog.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

AppVisibilitySettingsActivity::AppVisibilitySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("AppVisibilitySettings", renderer, mappedInput) {}

void AppVisibilitySettingsActivity::onEnter() {
  UiListActivity::onEnter();
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  app_catalog::refreshPlugins();
#endif
  dirty = false;
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  // Built-in apps and installed device plugins share this page but are different
  // things: the catalog lists the firmware apps first, then the SD packages.
  // Name the sections instead of leaving one flat list where a removable plugin
  // looks exactly like a built-in app.
  const int count = app_catalog::count();
  int firstPlugin = count;
  for (int i = 0; i < count; ++i) {
    if (app_catalog::isPlugin(i)) {
      firstPlugin = i;
      break;
    }
  }

  rowItems.clear();
  rowAppIndex.clear();
  rowItems.reserve(static_cast<size_t>(count) + 2);
  rowAppIndex.reserve(static_cast<size_t>(count) + 2);

  const auto addHeader = [this](StrId title) {
    fui::ListItem header;
    header.label = I18N.get(title);
    header.isHeader = true;
    header.actionValue = -1;
    rowItems.push_back(header);
    rowAppIndex.push_back(-1);
  };
  const auto addApp = [this](int appIndex) {
    fui::ListItem item;
    item.label = app_catalog::title(appIndex);
    item.actionValue = static_cast<int16_t>(rowItems.size());
    // Every row carries a leading glyph: without one the SDK reserves no icon
    // slot and the label starts left of its neighbours. Rows whose icon has no
    // Lucide mapping fall back to the generic blocks glyph.
    item.icon = listIconFor(app_catalog::at(appIndex).icon, 40);
    if (!item.icon) item.icon = fui::bitmapFromIcon(icon_blocks_48);
    rowItems.push_back(item);
    rowAppIndex.push_back(static_cast<int16_t>(appIndex));
  };

  if (firstPlugin > 0) addHeader(StrId::STR_APPS_TITLE);
  for (int i = 0; i < count; ++i) {
    if (i == firstPlugin) addHeader(StrId::STR_PLUGINS);
    addApp(i);
  }
}

void AppVisibilitySettingsActivity::onExit() {
  if (dirty) SETTINGS.saveToFile();
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  app_catalog::flushVisibility();
#endif
  Activity::onExit();
}

int AppVisibilitySettingsActivity::listCount() const { return static_cast<int>(rowItems.size()); }

bool AppVisibilitySettingsActivity::handleCustomInput() {
  if (!waitForConfirmRelease) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) waitForConfirmRelease = false;
  return true;
}

void AppVisibilitySettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  const int appIndex = rowAppIndex[static_cast<size_t>(index)];
  if (appIndex < 0) return;  // section header
  nav.selected = index;
  app.clearTapFlash();
  const bool visible = app_catalog::visible(appIndex);
  if (app_catalog::setVisible(appIndex, !visible)) {
    dirty = true;
    requestUpdate();
  }
}

const char* AppVisibilitySettingsActivity::headerTitle() const { return tr(STR_APP_VISIBILITY); }

void AppVisibilitySettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  const bool cpyRows = UITheme::getInstance().usesCpyLayout();
  // Same content margins as the Settings tabs: in CPY the list spans the screen
  // and the rows are inset so the scroll track lives in the card margin.
  const int16_t sideMargin = cpyRows ? 0 : content.x;
  const int16_t rightMargin =
      cpyRows ? 0 : static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(content.y), rightMargin,
                  static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), sideMargin});

  for (int i = 0; i < listCount(); ++i) {
    const int appIndex = rowAppIndex[static_cast<size_t>(i)];
    auto& row = rowItems[static_cast<size_t>(i)];
    if (appIndex < 0) {
      // Section header: text only, no switch or value.
      row.toggle = false;
      row.value = nullptr;
      continue;
    }
    GUI.setCheckboxRow(row, app_catalog::visible(appIndex));
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  props.rowGap = 0;
  props.headerText = screen.theme().smallText;
  props.headerText.bold = true;
  props.headerRowHeight = 44;
  props.sectionGap = 12;
  props.rowHeight = cpyRows ? 80 : static_cast<int16_t>(metrics.listRowHeight);
  if (cpyRows) {
    // Design frame 06 metrics, identical to SettingsActivity so this page reads
    // as part of the same system (rounded section cards, no row dividers, pill
    // switches).
    props.sidePadding = 24;
    props.valueInset = 0;
    props.iconSize = 40;
    props.textGap = 18;
    props.rowInset = 34;
    props.scrollIndicatorInset = 6;
    props.partialTrailingRow = true;
    props.partialTrailingMinHeight = 24;
    props.separator = fui::SeparatorStyle::None;
    props.toggleCheckbox = false;
    props.toggleWidth = 56;
    props.toggleHeight = 32;
    props.toggleRadius = 16;
    props.toggleKnobRadius = 12;
    props.toggleKnobInset = 3;
    props.toggleBorderWidth = 2;
  }
  const fui::Rect listBody = screen.body();
  listTop_ = listBody.y;
  listBottom_ = listBody.bottom();
  listRowH_ = props.rowHeight;
  syncListViewport(screen, props);
  screen.list(props);
}

void AppVisibilitySettingsActivity::drawFooter() {
  // Section card rings, drawn after the list pass so the row backgrounds cannot
  // cover the side strokes. Open ends run past the clip when a section
  // continues, exactly like the Settings tabs.
  if (UITheme::getInstance().usesCpyLayout() && listRowH_ > 0 && listBottom_ > listTop_) {
    const Rect content = pageContentRect();
    constexpr int kCardMargin = 32;
    constexpr int kHeaderH = 44;
    constexpr int kSectionGap = 12;
    const int rx = content.x + kCardMargin;
    const int rw = content.width - kCardMargin * 2;
    const int clipTop = listTop_;
    const int clipBottom = listBottom_;
    const GfxRenderer::ClipScope clip(renderer, content.x, clipTop, content.width, clipBottom - clipTop);
    const auto drawCard = [&](const int top, const int bottom, const bool roundTop, const bool roundBottom) {
      const int y0 = std::max(top, clipTop);
      const int y1 = std::min(bottom, clipBottom);
      if (y1 - y0 < 4) return;
      constexpr int kEndExtend = 24;
      const int drawTop = roundTop ? y0 : y0 - kEndExtend;
      const int drawBottom = roundBottom ? y1 : y1 + kEndExtend;
      renderer.drawRoundedRect(rx, drawTop, rw, drawBottom - drawTop, 2, 20, true);
      const int sideTop = y0 + (roundTop ? 18 : 0);
      const int sideBottom = y1 - (roundBottom ? 18 : 0);
      if (sideBottom > sideTop) {
        renderer.fillRect(rx, sideTop, 2, sideBottom - sideTop, true);
        renderer.fillRect(rx + rw - 2, sideTop, 2, sideBottom - sideTop, true);
      }
    };
    int cursor = clipTop;
    int groupStart = clipTop;
    bool groupHasRow = false;
    bool groupRoundTop = false;
    const int topIndex = activeNav().top;
    const int count = static_cast<int>(rowItems.size());
    for (int i = topIndex; i < count; ++i) {
      if (rowItems[i].isHeader) {
        if (groupHasRow) drawCard(groupStart, cursor, groupRoundTop, true);
        groupHasRow = false;
        if (i != topIndex) cursor += kSectionGap;
        if (cursor + kHeaderH > clipBottom) break;
        cursor += kHeaderH;
        groupStart = cursor;
        groupRoundTop = true;
        continue;
      }
      if (cursor + listRowH_ > clipBottom) {
        if (groupHasRow || groupRoundTop) drawCard(groupStart, clipBottom, groupRoundTop, false);
        break;
      }
      groupHasRow = true;
      cursor += listRowH_;
    }
    if (groupHasRow) drawCard(groupStart, cursor, groupRoundTop, true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
