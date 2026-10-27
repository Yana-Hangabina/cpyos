#include "FindBooksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WeReadStore.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/icons/inx_apps.h"
#include "fontIds.h"
#ifdef BOARD_HAS_PSRAM
#include "activities/apps/jjwxc/JjwxcActivity.h"
#include "activities/apps/jjwxc/JjwxcUpdates.h"
#endif
#ifdef ENABLE_CHINESE_VERSION
#include "activities/apps/weread/WeReadActivity.h"
#endif

namespace {

enum class Source : int {
  Jjwxc = 0,
  WeRead = 1,
  Local = 2,
  Opds = 3,
};

struct Entry {
  Source id;
  StrId title;
  StrId detail;
};

constexpr Entry kEntries[] = {
#ifdef ENABLE_CHINESE_VERSION
#ifdef BOARD_HAS_PSRAM
    {Source::Jjwxc, StrId::STR_JJWXC_TITLE, StrId::STR_JJWXC_LOGIN_HINT},
#endif
    {Source::WeRead, StrId::STR_WEREAD_TITLE, StrId::STR_WEREAD_SCAN_LOGIN},
#endif
    {Source::Local, StrId::STR_SOURCE_LOCAL, StrId::STR_FIND_LOCAL_DESC},
    {Source::Opds, StrId::STR_OPDS_BROWSER, StrId::STR_FIND_OTHER_DESC},
};

constexpr int kEntryCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

// Frame 08's geometry at 684 px: 32 px margins, a 88 px arrival row, then
// 172 px source cards with 16 px between them.
constexpr int16_t kSidePad = 32;
constexpr int16_t kTopMargin = 16;
constexpr int16_t kRowGap = 16;
constexpr int16_t kArrivalRowHeight = UiHighDpiProfile::enabled ? 88 : 64;
constexpr int16_t kCardHeight = UiHighDpiProfile::enabled ? 184 : 124;
constexpr int16_t kSealSize = UiHighDpiProfile::enabled ? 64 : 40;
constexpr int16_t kPillHeight = UiHighDpiProfile::enabled ? 56 : 40;
constexpr int16_t kPillPadX = UiHighDpiProfile::enabled ? 22 : 16;
constexpr int16_t kPillGap = 10;
constexpr int16_t kPadY = UiHighDpiProfile::enabled ? 18 : 12;  // card top/bottom inset

// The same white disc + 2 px ring the shelf's cover seals use, so a source is
// one visual thing across both tabs.
void drawChevron(const GfxRenderer& renderer, const int x, const int centerY) {
  renderer.drawLine(x, centerY - 7, x + 7, centerY, true);
  renderer.drawLine(x + 7, centerY, x, centerY + 7, true);
}

// Outlined capsule, the design's .pill.
void drawPill(const GfxRenderer& renderer, const Rect& rect, const char* label) {
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, rect.height / 2, Color::White);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, rect.height / 2, true);
  const int font = UI_10_FONT_ID;
  const int textW = renderer.getTextWidth(font, label);
  renderer.drawText(font, rect.x + (rect.width - textW) / 2, rect.y + (rect.height - renderer.getLineHeight(font)) / 2,
                    label);
}

}  // namespace

int FindBooksActivity::rowCount() const { return 1 + kEntryCount; }

FindBooksActivity::Action FindBooksActivity::primaryFor(const int row) {
  if (row == 0) return Action::Transfer;
  const int index = row - 1;
  if (index < 0 || index >= kEntryCount) return Action::None;
  switch (kEntries[index].id) {
    case Source::Jjwxc:
      return Action::JjwxcShelf;
    case Source::WeRead:
      return Action::WeRead;
    case Source::Local:
      return Action::LocalBrowser;
    case Source::Opds:
      return Action::Opds;
  }
  return Action::None;
}

FindBooksActivity::Action FindBooksActivity::secondaryFor(const int row) {
  const int index = row - 1;
  if (index < 0 || index >= kEntryCount) return Action::None;
  if (kEntries[index].id == Source::Local) return Action::LocalTransfer;
  if (kEntries[index].id == Source::Jjwxc) return Action::JjwxcDiscover;
  if (kEntries[index].id == Source::WeRead) return Action::WeReadAccount;
  return Action::None;
}

FindBooksActivity::Action FindBooksActivity::tertiaryFor(const int row) {
  const int index = row - 1;
  if (index < 0 || index >= kEntryCount) return Action::None;
  if (kEntries[index].id == Source::Jjwxc) return Action::JjwxcSearch;
  return Action::None;
}

FindBooksActivity::Action FindBooksActivity::quaternaryFor(const int row) {
  const int index = row - 1;
  if (index < 0 || index >= kEntryCount) return Action::None;
  if (kEntries[index].id == Source::Jjwxc) return Action::JjwxcAccount;
  return Action::None;
}

void FindBooksActivity::refreshSourceState() {
  jjwxcSession_ = false;
  wereadSession_ = false;
#ifdef BOARD_HAS_PSRAM
  jjwxcSession_ = jjwxcUpdates::sessionPresent();
#endif
#ifdef ENABLE_CHINESE_VERSION
  // Local evidence only: a readable session file. Whether the server still
  // accepts it is the app's answer to give, which is why the card never says
  // "已登录".
  WeReadStore::Session session;
  wereadSession_ = WeReadStore::loadSession(session) && session.valid();
#endif
}

void FindBooksActivity::onEnter() {
  Activity::onEnter();
  selected_ = 0;
  refreshSourceState();
  requestUpdate();
}

StrId FindBooksActivity::labelFor(const Action action) const {
  switch (action) {
    case Action::JjwxcShelf:
    case Action::WeRead:
      return StrId::STR_LIBRARY;
    case Action::JjwxcDiscover:
      return StrId::STR_JJWXC_DISCOVER;
    case Action::JjwxcSearch:
      return StrId::STR_JJWXC_SEARCH;
    case Action::JjwxcAccount:
      return jjwxcSession_ ? StrId::STR_JJWXC_ACCOUNT : StrId::STR_JJWXC_LOGIN;
    case Action::WeReadAccount:
      return wereadSession_ ? StrId::STR_WEREAD_TAB_MANAGE : StrId::STR_WEREAD_SCAN_LOGIN;
    case Action::LocalBrowser:
      return StrId::STR_BROWSE_FILES;
    case Action::LocalTransfer:
      return StrId::STR_FILE_TRANSFER;
    case Action::Opds:
      return StrId::STR_OPDS_SERVERS;
    default:
      return StrId::STR_OPEN;
  }
}

void FindBooksActivity::openJjwxc(const Action action) {
#ifdef ENABLE_CHINESE_VERSION
#ifdef BOARD_HAS_PSRAM
  JjwxcActivity::MenuShortcut shortcut = JjwxcActivity::MenuShortcut::Shelf;
  if (action == Action::JjwxcDiscover) shortcut = JjwxcActivity::MenuShortcut::Discover;
  if (action == Action::JjwxcSearch) shortcut = JjwxcActivity::MenuShortcut::Search;
  if (action == Action::JjwxcAccount) shortcut = JjwxcActivity::MenuShortcut::Account;
  JjwxcActivity::requestMenuShortcut(shortcut);
  auto activity = makeUniqueNoThrow<JjwxcActivity>(renderer, mappedInput);
  if (!activity) {
    JjwxcActivity::requestMenuShortcut(JjwxcActivity::MenuShortcut::None);
    return;
  }
  // Push, not replace: Back in the app must return to Find (the source card
  // the user came from). replaceActivityWith dropped Find, so its root Back
  // fell through to the app grid.
  activityManager.pushActivity(std::move(activity));
#endif
#endif
}

void FindBooksActivity::openAction(const Action action) {
  switch (action) {
    case Action::Transfer:
    case Action::LocalTransfer:
      activityManager.goToFileTransfer();
      return;
    case Action::JjwxcShelf:
    case Action::JjwxcDiscover:
    case Action::JjwxcSearch:
    case Action::JjwxcAccount:
      openJjwxc(action);
      return;
    case Action::WeRead:
#ifdef ENABLE_CHINESE_VERSION
      WeReadActivity::requestShortcut(WeReadActivity::Shortcut::None);
      activityManager.pushActivity(makeUniqueNoThrow<WeReadActivity>(renderer, mappedInput));
#endif
      return;
    case Action::WeReadAccount:
#ifdef ENABLE_CHINESE_VERSION
      WeReadActivity::requestShortcut(WeReadActivity::Shortcut::Manage);
      {
        auto activity = makeUniqueNoThrow<WeReadActivity>(renderer, mappedInput);
        if (!activity) {
          WeReadActivity::requestShortcut(WeReadActivity::Shortcut::None);
          return;
        }
        activityManager.pushActivity(std::move(activity));
      }
#endif
      return;
    case Action::LocalBrowser:
      activityManager.goToFileBrowser();
      return;
    case Action::Opds:
      activityManager.goToBrowser();
      return;
    case Action::None:
      return;
  }
}

void FindBooksActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selected_ = std::max(0, selected_ - 1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selected_ = std::min(rowCount() - 1, selected_ + 1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openAction(primaryFor(selected_));
    return;
  }

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    // Pills first: they sit inside a card, and a tap inside one must not fall
    // through to the card's own destination.
    for (int row = 0; row < rowCount(); row++) {
      for (int slot = 0; slot < kMaxPills; slot++) {
        const Rect& pill = pillRects_[row][slot];
        if (pill.width > 0 && x >= pill.x && x < pill.x + pill.width && y >= pill.y &&
            y < pill.y + pill.height) {
          selected_ = row;
          openAction(pillActions_[row][slot]);
          return;
        }
      }
    }
    for (int row = 0; row < rowCount(); row++) {
      const Rect& rect = rowRects_[row];
      if (rect.width > 0 && x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) {
        selected_ = row;
        openAction(rowActions_[row]);
        return;
      }
    }
  }
}

void FindBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_FIND_BOOKS));

  for (int i = 0; i < kMaxRows; i++) {
    rowRects_[i] = Rect{};
    for (int slot = 0; slot < kMaxPills; slot++) {
      pillRects_[i][slot] = Rect{};
      pillActions_[i][slot] = Action::None;
    }
    rowActions_[i] = Action::None;
  }

  const Rect content = pageContentRect();
  const int left = content.x + kSidePad;
  const int width = std::max(1, content.width - 2 * kSidePad);
  const int n = kEntryCount;

  constexpr int titleFont = UI_12_FONT_ID;
  constexpr int detailFont = UI_10_FONT_ID;
  const int titleLine = renderer.getLineHeight(titleFont);
  const int detailLine = renderer.getLineHeight(detailFont);
  // A card must hold its header block (seal / title / status) and the pill row
  // with air between them; anything less is squeezed by the band, never clipped.
  const int headerBlock = std::max<int>(kSealSize, titleLine + 3 + detailLine);
  const int minCardHeight = kPadY * 2 + headerBlock + 8 + kPillHeight;

  // Cards share what the band can give them; with four sources that is the
  // design's 172 px, with two (no Chinese apps) they simply keep that height.
  const int available = content.height - 2 * kTopMargin - kArrivalRowHeight - kRowGap;
  const int cardHeight =
      std::min<int>(kCardHeight, std::max(minCardHeight, (available - kRowGap * (n - 1)) / std::max(1, n)));

  // Top row: the way books arrive on the card. There is no inbox on this
  // firmware (uploads land straight in their target folder), so the row is the
  // upload page itself and carries no "N waiting" count or timestamp.
  {
    const Rect row{left, content.y + kTopMargin, width, kArrivalRowHeight};
    renderer.fillRoundedRect(row.x, row.y, row.width, row.height, 16, Color::White);
    renderer.drawRoundedRect(row.x, row.y, row.width, row.height, 2, 16, true);
    if (selected_ == 0) renderer.drawRoundedRect(row.x - 2, row.y - 2, row.width + 4, row.height + 4, 2, 18, true);
    const int iconSize = 32;
    InxAppIcons::drawModern(renderer, UIIcon::Transfer, row.x + 20, row.y + (row.height - iconSize) / 2, iconSize,
                            false);
    const int textX = row.x + 20 + iconSize + 16;
    const int chevX = row.x + row.width - 22;
    drawChevron(renderer, chevX, row.y + row.height / 2);
    const int textRight = chevX - 16;
    const int blockH = titleLine + 2 + detailLine;
    const int textY = row.y + (row.height - blockH) / 2;
    renderer.drawText(titleFont, textX, textY, tr(STR_WEB_TRANSFER), true, EpdFontFamily::BOLD);
    const std::string detail = renderer.truncatedText(detailFont, tr(STR_WEB_TRANSFER_DESC),
                                                      std::max(1, textRight - textX));
    renderer.drawText(detailFont, textX, textY + titleLine + 2, detail.c_str());
    rowRects_[0] = row;
    rowActions_[0] = Action::Transfer;
  }

  int rowY = content.y + kTopMargin + kArrivalRowHeight + kRowGap;
  for (int i = 0; i < n; i++) {
    const Entry& entry = kEntries[i];
    const int row = i + 1;
    const Rect card{left, rowY, width, cardHeight};
    renderer.fillRoundedRect(card.x, card.y, card.width, card.height, 16, Color::White);
    renderer.drawRoundedRect(card.x, card.y, card.width, card.height, 2, 16, true);
    if (selected_ == row) renderer.drawRoundedRect(card.x - 2, card.y - 2, card.width + 4, card.height + 4, 2, 18, true);

    // Header: seal (or the local/OPDS glyph), name, honest status line.
    const int sealX = card.x + 20;
    const int sealY = card.y + kPadY;
    const int textX = sealX + kSealSize + 16;
    const int chevX = card.x + card.width - 22;
    drawChevron(renderer, chevX, sealY + kSealSize / 2);
    const int textRight = chevX - 16;

    const char* title = I18N.get(entry.title);
    // Full seal: the painted source logos carry fine strokes and looked muddy
    // when drawn as a 48 px inner square.
    switch (entry.id) {
      case Source::Jjwxc:
        InxAppIcons::drawModern(renderer, UIIcon::Jjwxc, sealX, sealY, kSealSize, false);
        break;
      case Source::WeRead:
        InxAppIcons::drawModern(renderer, UIIcon::WeRead, sealX, sealY, kSealSize, false);
        break;
      case Source::Local:
        InxAppIcons::drawModern(renderer, UIIcon::File, sealX, sealY, kSealSize, false);
        break;
      case Source::Opds:
        InxAppIcons::drawModern(renderer, UIIcon::Opds, sealX, sealY, kSealSize, false);
        break;
    }
    const std::string titleShown =
        renderer.truncatedText(titleFont, title, std::max(1, textRight - textX), EpdFontFamily::BOLD);
    renderer.drawText(titleFont, textX, sealY, titleShown.c_str(), true, EpdFontFamily::BOLD);

    // Status: only what the firmware can verify locally. The lamp is filled
    // for "a session file is on the card", hollow for "none" — never a claim
    // about the server.
    const char* status = nullptr;
    bool hasLamp = false;
    bool lampOn = false;
    switch (entry.id) {
      case Source::Jjwxc:
#ifdef BOARD_HAS_PSRAM
        status = jjwxcSession_ ? tr(STR_SESSION_SAVED) : tr(STR_JJWXC_NOT_LOGGED_IN);
        hasLamp = true;
        lampOn = jjwxcSession_;
#else
        status = I18N.get(entry.detail);
#endif
        break;
      case Source::WeRead:
#ifdef ENABLE_CHINESE_VERSION
        status = wereadSession_ ? tr(STR_SESSION_SAVED) : tr(STR_WEREAD_SCAN_LOGIN);
        hasLamp = true;
        lampOn = wereadSession_;
#else
        status = I18N.get(entry.detail);
#endif
        break;
      default:
        status = I18N.get(entry.detail);
        break;
    }

    const int statusY = sealY + titleLine + 3;
    int statusX = textX;
    if (hasLamp) {
      constexpr int lamp = 16;
      const int lampX = statusX;
      const int lampY = statusY + (detailLine - lamp) / 2;
      if (lampOn) {
        renderer.fillRoundedRect(lampX, lampY, lamp, lamp, lamp / 2, Color::Black);
      } else {
        renderer.fillRoundedRect(lampX, lampY, lamp, lamp, lamp / 2, Color::White);
        renderer.drawRoundedRect(lampX, lampY, lamp, lamp, 2, lamp / 2, true);
      }
      statusX += lamp + 8;
    }
    const std::string statusShown =
        renderer.truncatedText(detailFont, status, std::max(1, textRight - statusX));
    renderer.drawText(detailFont, statusX, statusY, statusShown.c_str());

    // Pills: the card's real entry points. JJWXC exposes shelf, discovery,
    // search and login/account directly instead of hiding them one level down
    // in the app menu. Two to four pills share the card's text column.
    const int pillY = card.y + card.height - kPillHeight - kPadY;
    int pillX = textX;
    const Action actions[kMaxPills] = {primaryFor(row), secondaryFor(row), tertiaryFor(row),
                                       quaternaryFor(row)};
    for (int slot = 0; slot < kMaxPills; ++slot) {
      if (actions[slot] == Action::None) continue;
      const char* label = I18N.get(labelFor(actions[slot]));
      const int pillW = renderer.getTextWidth(detailFont, label) + 2 * kPillPadX;
      if (pillX + pillW > card.x + card.width - 20) break;
      const Rect pill{pillX, pillY, pillW, kPillHeight};
      drawPill(renderer, pill, label);
      pillRects_[row][slot] = Rect{pill.x - 2, static_cast<int16_t>(pill.y - kPadY),
                                   static_cast<int16_t>(pill.width + 4),
                                   static_cast<int16_t>(kPillHeight + 2 * kPadY)};
      pillActions_[row][slot] = actions[slot];
      pillX += pillW + kPillGap;
    }

    rowRects_[row] = card;
    // The card's body opens the same place as its primary pill; a tap on a
    // pill is handled before this by loop().
    rowActions_[row] = actions[0];
    rowY += cardHeight + kRowGap;
  }

  // The source cards stop well above the tab bar; the design closes the page
  // with a quiet footnote instead of letting the stack float in white space.
  const int noteY = content.y + content.height - 44;
  if (UITheme::getInstance().usesCpyLayout() && noteY > rowY + 8) {
    const std::string note =
        renderer.truncatedText(SMALL_FONT_ID, tr(STR_FIND_FOOTNOTE), std::max(1, width));
    renderer.drawText(SMALL_FONT_ID, left, noteY, note.c_str());
  }

  renderer.displayBuffer();
}
