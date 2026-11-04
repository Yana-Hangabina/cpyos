#include "AppCatalog.h"

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace {

using appVisibility::AppId;
using appVisibility::appBit;

void openFileTransfer() { activityManager.goToFileTransfer(); }
void openOpdsBrowser() { activityManager.goToBrowser(); }
void openReadingStats() { activityManager.goToReadingStatsMenu(); }
void openSudoku() { activityManager.goToSudoku(); }
void openGomoku() { activityManager.goToGomoku(); }
void openSokoban() { activityManager.goToSokoban(); }
void openMinesweeper() { activityManager.goToMinesweeper(); }
void openGame2048() { activityManager.goToGame2048(); }
void openUglyAvatar() { activityManager.goToUglyAvatar(); }
void openStandby() { activityManager.goToStandby(); }
void openAirPage() { activityManager.goToAirPage(); }
void openBuddy() { activityManager.goToBuddy(); }
void openPixelSwitch() { activityManager.goToPixelSwitch(); }
void openCalculator() { activityManager.goToCalculator(); }
void openWoodfish() { activityManager.goToWoodfish(); }
#ifdef ENABLE_CHINESE_VERSION
void openWeRead() { activityManager.goToWeRead(); }
void openChineseChess() { activityManager.goToChineseChess(); }
#ifdef BOARD_HAS_PSRAM
void openJjwxc() { activityManager.goToJjwxc(); }
void openWebBrowser() { activityManager.goToWebBrowser(); }
#endif
#endif

constexpr app_catalog::Entry kEntries[] = {
    {AppId::FileTransfer, StrId::STR_FILE_TRANSFER, UIIcon::Transfer, &openFileTransfer},
    {AppId::OpdsBrowser, StrId::STR_OPDS_BROWSER, UIIcon::Opds, &openOpdsBrowser},
#ifdef ENABLE_CHINESE_VERSION
    {AppId::WeRead, StrId::STR_WEREAD_TITLE, UIIcon::WeRead, &openWeRead},
#ifdef BOARD_HAS_PSRAM
    // Requires PSRAM: the catalogue scratch buffer and the chapter/shelf vectors
    // are far larger than the C3's ~380 KB internal-RAM budget, and a failed
    // allocation aborts under -fno-exceptions rather than degrading.
    {AppId::Jjwxc, StrId::STR_JJWXC_TITLE, UIIcon::Jjwxc, &openJjwxc},
    // The browser keeps a 2 MiB fetch buffer and a PSRAM-backed document; the
    // same -fno-exceptions abort applies to C3's internal-RAM budget.
    {AppId::WebBrowser, StrId::STR_BROWSER_TITLE, UIIcon::Browser, &openWebBrowser},
#endif
#endif
    {AppId::AirPage, StrId::STR_AIRPAGE_TITLE, UIIcon::AirPage, &openAirPage},
    {AppId::ReadingStats, StrId::STR_READING_STATS, UIIcon::ReadingStats, &openReadingStats},
    {AppId::Sudoku, StrId::STR_SUDOKU_TITLE, UIIcon::Sudoku, &openSudoku},
    {AppId::Gomoku, StrId::STR_GOMOKU_TITLE, UIIcon::Gomoku, &openGomoku},
    {AppId::Sokoban, StrId::STR_SOKOBAN_TITLE, UIIcon::Sokoban, &openSokoban},
#ifdef ENABLE_CHINESE_VERSION
    {AppId::ChineseChess, StrId::STR_CHINESE_CHESS_TITLE, UIIcon::ChineseChess, &openChineseChess},
#endif
    {AppId::Minesweeper, StrId::STR_MINESWEEPER_TITLE, UIIcon::Minesweeper, &openMinesweeper},
    {AppId::Game2048, StrId::STR_2048_TITLE, UIIcon::Game2048, &openGame2048},
    {AppId::UglyAvatar, StrId::STR_UGLY_AVATAR, UIIcon::Avatar, &openUglyAvatar},
    {AppId::Buddy, StrId::STR_BUDDY_TITLE, UIIcon::Buddy, &openBuddy},
    {AppId::PixelSwitch, StrId::STR_PIXEL_SWITCH_TITLE, UIIcon::PixelSwitch, &openPixelSwitch},
    {AppId::Calculator, StrId::STR_CALCULATOR_TITLE, UIIcon::Calculator, &openCalculator},
    {AppId::Woodfish, StrId::STR_WOODFISH_TITLE, UIIcon::Woodfish, &openWoodfish},
    {AppId::Standby, StrId::STR_STANDBY_TITLE, UIIcon::Standby, &openStandby},
};

constexpr int kCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

constexpr bool idsAreUnique() {
  for (int i = 0; i < kCount; ++i) {
    for (int j = i + 1; j < kCount; ++j) {
      if (kEntries[i].id == kEntries[j].id) return false;
    }
  }
  return true;
}

static_assert(kCount <= 32, "the app catalog must fit hiddenAppsMask");
static_assert(static_cast<uint8_t>(appVisibility::AppId::Count) <= 32,
              "hiddenAppsMask supports at most 32 stable app IDs");
static_assert(idsAreUnique(), "stable app IDs must not be reused");

}  // namespace

namespace app_catalog {

int count() { return kCount; }

const Entry& at(const int index) {
  const int clamped = index < 0 ? 0 : (index >= kCount ? kCount - 1 : index);
  return kEntries[clamped];
}

StrId titleId(const int index) { return at(index).titleId; }

bool visible(const int index) {
  if (index < 0 || index >= kCount) return false;
  return (SETTINGS.hiddenAppsMask & appBit(kEntries[index].id)) == 0;
}

bool setVisible(const int index, const bool show) {
  if (index < 0 || index >= kCount) return false;
  const uint32_t bit = appBit(kEntries[index].id);
  const uint32_t updated = show ? SETTINGS.hiddenAppsMask & ~bit : SETTINGS.hiddenAppsMask | bit;
  if (updated == SETTINGS.hiddenAppsMask) return false;
  SETTINGS.hiddenAppsMask = updated;
  return true;
}

uint32_t effectiveHiddenMask(const uint32_t hiddenMask, const bool hasOpdsServers) {
  uint32_t effective = hiddenMask;
  if (!hasOpdsServers) effective |= appBit(AppId::OpdsBrowser);
  return effective;
}

int visibleCount(const uint32_t hiddenMask, const bool hasOpdsServers) {
  const uint32_t mask = effectiveHiddenMask(hiddenMask, hasOpdsServers);
  int visibleCount = 0;
  for (const auto& entry : kEntries) {
    if ((mask & appBit(entry.id)) == 0) ++visibleCount;
  }
  return visibleCount;
}

int visibleToAppIndex(const uint32_t hiddenMask, const bool hasOpdsServers, const int visibleIndex) {
  const uint32_t mask = effectiveHiddenMask(hiddenMask, hasOpdsServers);
  int visible = 0;
  for (int i = 0; i < kCount; ++i) {
    if ((mask & appBit(kEntries[i].id)) != 0) continue;
    if (visible++ == visibleIndex) return i;
  }
  return -1;
}

}  // namespace app_catalog
