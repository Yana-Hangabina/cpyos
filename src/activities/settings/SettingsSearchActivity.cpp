#include "SettingsSearchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <memory>

#include "MappedInputManager.h"
#include "SettingsList.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/listIcons48.h"
#include "components/icons/search32.h"
#include "fontIds.h"

namespace {
constexpr int kRowH = 104;

int categoryIndex(const StrId category) {
  if (category == StrId::STR_CAT_DISPLAY) return 0;
  if (category == StrId::STR_CAT_READER) return 1;
  if (category == StrId::STR_CAT_CONTROLS) return 2;
  if (category == StrId::STR_CAT_SYSTEM) return 3;
  return -1;
}

std::string fold(const char* text) {
  std::string out;
  if (text == nullptr) return out;
  for (const char* p = text; *p; ++p) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
  return out;
}

void drawMaskIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const int size) {
  const int rowBytes = (icon.w + 7) / 8;
  for (int row = 0; row < size; ++row) {
    const int sourceY = row * icon.h / size;
    const uint8_t* sourceRow = icon.bits + sourceY * rowBytes;
    for (int column = 0; column < size; ++column) {
      const int sourceX = column * icon.w / size;
      if (((sourceRow[sourceX / 8] >> (7 - (sourceX & 7))) & 1U) == 0) {
        renderer.drawPixel(x + column, y + row, true);
      }
    }
  }
}
}  // namespace

void SettingsSearchActivity::onEnter() {
  Activity::onEnter();
  if (query_.empty()) {
    prompt();
  } else {
    buildResults();
    requestUpdate(true);
  }
}

void SettingsSearchActivity::prompt() {
  const auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    query_ = std::get<KeyboardResult>(result.data).text;
    buildResults();
    selected_ = 0;
    scroll_ = 0;
    requestUpdate(true);
  };
  if (!startActivityForResultWith<KeyboardEntryActivity>(handler, tr(STR_SEARCH_SETTINGS), "", 32,
                                                         InputType::Text)) {
    finish();
  }
}

void SettingsSearchActivity::buildResults() {
  hits_.clear();
  const std::string needle = fold(query_.c_str());
  if (needle.empty()) return;
  for (const auto& setting : getSettingsList()) {
    const int category = categoryIndex(setting.category);
    if (category < 0) continue;
    const std::string label = I18N.get(setting.nameId);
    const std::string categoryName = I18N.get(setting.category);
    if (fold(label.c_str()).find(needle) == std::string::npos &&
        fold(categoryName.c_str()).find(needle) == std::string::npos) {
      continue;
    }
    hits_.push_back(Hit{setting.nameId, category});
    if (hits_.size() >= 24) break;
  }
}

void SettingsSearchActivity::activate(const int index) {
  if (index < 0 || index >= static_cast<int>(hits_.size())) return;
  activityManager.pushActivity(std::make_unique<SettingsActivity>(
      renderer, mappedInput, hits_[static_cast<size_t>(index)].category, /*subpage=*/true));
}

void SettingsSearchActivity::loop() {
  const int contentTop = pageContentRect().y + 8 + 56 + 16;
  const int rows = std::max(1, (pageContentRect().y + pageContentRect().height - contentTop) / kRowH);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(scroll_ + selected_);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
    if (hits_.empty()) return;
    if (selected_ + 1 < rows && scroll_ + selected_ + 1 < static_cast<int>(hits_.size())) {
      ++selected_;
    } else if (scroll_ + rows < static_cast<int>(hits_.size())) {
      ++scroll_;
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    if (selected_ > 0) {
      --selected_;
    } else if (scroll_ > 0) {
      --scroll_;
    }
    requestUpdate();
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (y < contentTop) return;
    const int row = (y - contentTop) / kRowH;
    if (row >= 0 && row < rows) activate(scroll_ + row);
  }
}

void SettingsSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_SEARCH_SETTINGS));

  const Rect content = pageContentRect();
  const int x = content.x + 40;
  const int w = content.width - 80;
  int y = content.y + 8;

  // Search field, matching the My page's rounded field (icon + query) instead
  // of a bare query line over a rule.
  constexpr int kFieldH = 56;
  const Rect field{x, y, w, kFieldH};
  renderer.drawRoundedRect(field.x, field.y, field.width, field.height, 2, 16, true);
  constexpr int kIcon = 24;
  drawMaskIcon(renderer, icon_search_32, field.x + 16, field.y + (kFieldH - kIcon) / 2, kIcon);
  renderer.drawText(UI_10_FONT_ID, field.x + 52,
                    field.y + (kFieldH - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                    query_.empty() ? tr(STR_SEARCH_SETTINGS) : query_.c_str());
  y += kFieldH + 16;

  if (hits_.empty()) {
    renderer.drawText(UI_10_FONT_ID, x, y + 20, tr(STR_LOCAL_NO_MATCHES));
    renderer.displayBuffer();
    return;
  }

  const int rows = std::max(1, (content.y + content.height - y) / kRowH);
  for (int i = 0; i < rows; ++i) {
    const int index = scroll_ + i;
    if (index >= static_cast<int>(hits_.size())) break;
    const Rect row{x, y + i * kRowH, w, kRowH};
    // Each hit is its own card, the same language as My/Settings. The old
    // selected-only ring left the category text sitting on the frame line.
    renderer.drawRoundedRect(row.x - 2, row.y + 2, row.width + 4, row.height - 4,
                             i == selected_ ? 3 : 2, 12, true);
    const auto& hit = hits_[static_cast<size_t>(index)];
    renderer.drawText(UI_12_FONT_ID, row.x + 18, row.y + 18, I18N.get(hit.label), true, EpdFontFamily::BOLD);
    static const StrId kCategoryNames[4] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                            StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};
    renderer.drawText(SMALL_FONT_ID, row.x + 18, row.y + 56,
                      I18N.get(kCategoryNames[std::clamp(hit.category, 0, 3)]));
  }
  renderer.displayBuffer();
}
