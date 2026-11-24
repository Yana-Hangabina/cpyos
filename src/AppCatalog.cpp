#include "AppCatalog.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "Logging.h"
#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

#if defined(CROSSPOINT_PLUGIN_RUNTIME)
#include <Arduino.h>
#include <HalStorage.h>

#include "plugins/PluginManager.h"
#endif

namespace {

using appVisibility::AppId;
using appVisibility::appBit;

void openFileTransfer() { activityManager.goToFileTransfer(); }
void openOpdsBrowser() { activityManager.goToBrowser(); }
void openReadingStats() { activityManager.goToReadingStatsMenu(); }
#if !defined(CROSSPOINT_PLUGIN_RUNTIME)
void openSudoku() { activityManager.goToSudoku(); }
void openGomoku() { activityManager.goToGomoku(); }
void openSokoban() { activityManager.goToSokoban(); }
void openMinesweeper() { activityManager.goToMinesweeper(); }
void openGame2048() { activityManager.goToGame2048(); }
#endif
void openUglyAvatar() { activityManager.goToUglyAvatar(); }
void openStandby() { activityManager.goToStandby(); }
void openAirPage() { activityManager.goToAirPage(); }
void openBuddy() { activityManager.goToBuddy(); }
void openPixelSwitch() { activityManager.goToPixelSwitch(); }
void openCalculator() { activityManager.goToCalculator(); }
void openWoodfish() { activityManager.goToWoodfish(); }
#if defined(CROSSPOINT_PLUS_PORT)
void openCamera() { activityManager.goToCamera(); }
void openVoiceMemo() { activityManager.goToVoiceMemo(); }
void openVoiceClient() { activityManager.goToVoiceClient(); }
#endif
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
void openNativePlugins() { activityManager.goToNativePlugins(); }
#endif
#ifdef ENABLE_CHINESE_VERSION
void openWeRead() { activityManager.goToWeRead(); }
#if !defined(CROSSPOINT_PLUGIN_RUNTIME)
void openChineseChess() { activityManager.goToChineseChess(); }
#endif
#if defined(BOARD_HAS_PSRAM) && defined(CROSSPOINT_BOOKSOURCE_JJWXC)
void openJjwxc() { activityManager.goToJjwxc(); }
#endif
#ifdef BOARD_HAS_PSRAM
void openWebBrowser() { activityManager.goToWebBrowser(); }
#endif
#endif

constexpr app_catalog::Entry kEntries[] = {
    {AppId::FileTransfer, StrId::STR_FILE_TRANSFER, UIIcon::Transfer, &openFileTransfer},
    {AppId::OpdsBrowser, StrId::STR_OPDS_BROWSER, UIIcon::Opds, &openOpdsBrowser},
#ifdef ENABLE_CHINESE_VERSION
    {AppId::WeRead, StrId::STR_WEREAD_TITLE, UIIcon::WeRead, &openWeRead},
#if defined(BOARD_HAS_PSRAM) && defined(CROSSPOINT_BOOKSOURCE_JJWXC)
    // Requires PSRAM and the private source component. Advertising the tile
    // when the component is absent turned a normal "not built" image into a
    // dead tap: goToJjwxc() returns silently with a null source.
    {AppId::Jjwxc, StrId::STR_JJWXC_TITLE, UIIcon::Jjwxc, &openJjwxc},
#endif
#ifdef BOARD_HAS_PSRAM
    // The browser keeps a 2 MiB fetch buffer and a PSRAM-backed document; the
    // same -fno-exceptions abort applies to C3's internal-RAM budget.
    {AppId::WebBrowser, StrId::STR_BROWSER_TITLE, UIIcon::Browser, &openWebBrowser},
#endif
#endif
    {AppId::AirPage, StrId::STR_AIRPAGE_TITLE, UIIcon::AirPage, &openAirPage},
    {AppId::ReadingStats, StrId::STR_READING_STATS, UIIcon::ReadingStats, &openReadingStats},
#if !defined(CROSSPOINT_PLUGIN_RUNTIME)
    {AppId::Sudoku, StrId::STR_SUDOKU_TITLE, UIIcon::Sudoku, &openSudoku},
    {AppId::Gomoku, StrId::STR_GOMOKU_TITLE, UIIcon::Gomoku, &openGomoku},
    {AppId::Sokoban, StrId::STR_SOKOBAN_TITLE, UIIcon::Sokoban, &openSokoban},
#endif
#ifdef ENABLE_CHINESE_VERSION
#if !defined(CROSSPOINT_PLUGIN_RUNTIME)
    {AppId::ChineseChess, StrId::STR_CHINESE_CHESS_TITLE, UIIcon::ChineseChess, &openChineseChess},
#endif
#endif
#if !defined(CROSSPOINT_PLUGIN_RUNTIME)
    {AppId::Minesweeper, StrId::STR_MINESWEEPER_TITLE, UIIcon::Minesweeper, &openMinesweeper},
    {AppId::Game2048, StrId::STR_2048_TITLE, UIIcon::Game2048, &openGame2048},
#endif
    {AppId::UglyAvatar, StrId::STR_UGLY_AVATAR, UIIcon::Avatar, &openUglyAvatar},
    {AppId::Buddy, StrId::STR_BUDDY_TITLE, UIIcon::Buddy, &openBuddy},
    {AppId::PixelSwitch, StrId::STR_PIXEL_SWITCH_TITLE, UIIcon::PixelSwitch, &openPixelSwitch},
    {AppId::Calculator, StrId::STR_CALCULATOR_TITLE, UIIcon::Calculator, &openCalculator},
    {AppId::Woodfish, StrId::STR_WOODFISH_TITLE, UIIcon::Woodfish, &openWoodfish},
    {AppId::Standby, StrId::STR_STANDBY_TITLE, UIIcon::Standby, &openStandby},
#if defined(CROSSPOINT_PLUS_PORT)
    // Metalio E-INK4 Plus only: the board is the one target with a GC2145 DVP
    // sensor. Reuses the existing "images" label and the Image icon; a new i18n
    // key would not fit the 15-bit English blob budget.
    {AppId::Camera, StrId::STR_IMAGES, UIIcon::Image, &openCamera},
    // Voice memo: the Plus is also the only target with the BH1098 microphone
    // path. UIIcon has no audio glyph and BaseTheme.h is outside this port's
    // edit scope, so the tile is drawn with the existing Lucide music note in
    // MyActivity; the value below is only what the visibility list falls back to.
    {AppId::VoiceMemo, StrId::STR_VOICE_MEMO, UIIcon::Text, &openVoiceMemo},
    // AI voice client (push-to-talk over a xiaozhi WebSocket). Like the memo
    // tile it has no UIIcon glyph; MyActivity substitutes the Lucide speaker.
    {AppId::VoiceClient, StrId::STR_VOICE_CLIENT, UIIcon::Text, &openVoiceClient},
#endif
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
    // Out-of-tree native ELF packages on the SD card. The tile reuses the
    // existing plugins label and the generic app-grid glyph; no new i18n key
    // fits the 15-bit English blob.
    {AppId::NativePlugins, StrId::STR_PLUGINS, UIIcon::Apps, &openNativePlugins},
#endif
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

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

// Installed plugins are dynamic rows. Their hidden state is keyed by plugin id
// rather than by one of the 32 stable app bits: installing or removing a package
// must never change which bit another package uses.
constexpr const char* kVisibilityPath = "/.crosspoint/plugins/.visibility";
constexpr const char* kVisibilityMagic = "PXHV1";

struct PluginRow {
  std::string id;
  std::string name;
};

std::vector<PluginRow> gPlugins;
std::vector<app_catalog::Entry> gEntries;
std::vector<std::string> gHiddenPlugins;
bool gReady = false;
bool gDirty = true;
bool gHiddenDirty = false;

// The six game packages are externalized versions of the built-in tiles, so
// reuse their localized names and glyphs instead of showing raw manifest
// strings (which are English-only and longer than a 3-column tile label).
StrId pluginTitleId(const std::string& id) {
  if (id == "2048") return StrId::STR_2048_TITLE;
  if (id == "sokoban") return StrId::STR_SOKOBAN_TITLE;
  if (id == "gomoku") return StrId::STR_GOMOKU_TITLE;
  if (id == "minesweeper") return StrId::STR_MINESWEEPER_TITLE;
  if (id == "sudoku") return StrId::STR_SUDOKU_TITLE;
  if (id == "chinese-chess") return StrId::STR_CHINESE_CHESS_TITLE;
  return StrId::STR_NONE_OPT;
}

UIIcon pluginIcon(const std::string& id) {
  if (id == "2048") return UIIcon::Game2048;
  if (id == "sokoban") return UIIcon::Sokoban;
  if (id == "gomoku") return UIIcon::Gomoku;
  if (id == "minesweeper") return UIIcon::Minesweeper;
  if (id == "sudoku") return UIIcon::Sudoku;
  if (id == "chinese-chess") return UIIcon::ChineseChess;
  return UIIcon::Apps;
}

bool pluginHidden(const std::string& id) {
  return std::find(gHiddenPlugins.begin(), gHiddenPlugins.end(), id) != gHiddenPlugins.end();
}

void loadHiddenPlugins() {
  gHiddenPlugins.clear();
  std::string text;
  if (!Storage.readFileToString("APPS", kVisibilityPath, 4096, text)) return;

  size_t pos = 0;
  bool first = true;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line != kVisibilityMagic) return;  // unknown format: treat as visible
    } else if (!line.empty() && native_plugins::isSafePluginId(line) && !pluginHidden(line)) {
      gHiddenPlugins.push_back(std::move(line));
    }
    if (end == text.size()) break;
    pos = end + 1;
  }
}

app_catalog::Entry pluginEntry(const int index) {
  const auto& plugin = gPlugins[static_cast<size_t>(index)];
  return {AppId::NativePlugins, pluginTitleId(plugin.id), pluginIcon(plugin.id), nullptr,
          static_cast<int16_t>(index)};
}

void ensureReady() {
  if (!gReady) {
    gEntries.assign(kEntries, kEntries + kCount);
    gReady = true;
  }
  if (gDirty) app_catalog::refreshPlugins();
}

#endif  // CROSSPOINT_PLUGIN_RUNTIME

}  // namespace

namespace app_catalog {

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

void refreshPlugins() {
  // The visibility page writes the file before returning; reload it so the
  // tool drawer, the visibility page and a fresh boot all use the same source.
  loadHiddenPlugins();
  auto scanned = native_plugins::scan();
  gPlugins.clear();
  for (auto& plugin : scanned) {
    if (!plugin.error.empty()) continue;
    PluginRow row;
    row.id = plugin.id;
    row.name = plugin.name.empty() ? plugin.id : plugin.name;
    gPlugins.push_back(std::move(row));
  }
  std::sort(gPlugins.begin(), gPlugins.end(),
            [](const PluginRow& a, const PluginRow& b) { return a.id < b.id; });

  gEntries.assign(kEntries, kEntries + kCount);
  for (int i = 0; i < static_cast<int>(gPlugins.size()); ++i) {
    gEntries.push_back(pluginEntry(i));
  }
  gReady = true;
  gDirty = false;
  LOG_INF("APPS", "catalog: %u plugins, %u hidden", static_cast<unsigned>(gPlugins.size()),
          static_cast<unsigned>(gHiddenPlugins.size()));
}

void invalidatePlugins() { gDirty = true; }

bool pluginsDirty() { return gDirty; }

const char* pluginTitle(const char* id) {
  if (id == nullptr) return nullptr;
  const StrId titleId = pluginTitleId(id);
  return titleId == StrId::STR_NONE_OPT ? nullptr : I18N.get(titleId);
}

void flushVisibility() {
  if (!gHiddenDirty) return;
  std::string content = kVisibilityMagic;
  content += '\n';
  for (const auto& id : gHiddenPlugins) {
    content += id;
    content += '\n';
  }
  if (!Storage.ensureDirectoryExists(native_plugins::kPluginRoot)) {
    LOG_ERR("APPS", "cannot create plugin root for visibility state");
    return;
  }
  const bool ok = Storage.writeFile(kVisibilityPath, String(content.c_str()));
  if (ok) gHiddenDirty = false;
  LOG_INF("APPS", "plugin visibility: %u hidden (%s)", static_cast<unsigned>(gHiddenPlugins.size()),
          ok ? "saved" : "write failed");
}

int count() {
  ensureReady();
  return static_cast<int>(gEntries.size());
}

const Entry& at(const int index) {
  ensureReady();
  const int clamped = index < 0 ? 0 : (index >= static_cast<int>(gEntries.size())
                                          ? static_cast<int>(gEntries.size()) - 1
                                          : index);
  return gEntries[static_cast<size_t>(clamped)];
}

#else

int count() { return kCount; }

const Entry& at(const int index) {
  const int clamped = index < 0 ? 0 : (index >= kCount ? kCount - 1 : index);
  return kEntries[clamped];
}

#endif  // CROSSPOINT_PLUGIN_RUNTIME

const char* title(const int index) {
  const Entry& entry = at(index);
  if (entry.titleId != StrId::STR_NONE_OPT) return I18N.get(entry.titleId);
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  if (entry.pluginIndex >= 0 && entry.pluginIndex < static_cast<int>(gPlugins.size())) {
    return gPlugins[static_cast<size_t>(entry.pluginIndex)].name.c_str();
  }
#endif
  return I18N.get(entry.titleId);
}

StrId titleId(const int index) { return at(index).titleId; }

bool isPlugin(const int index) {
  if (index < 0 || index >= count()) return false;
  return at(index).pluginIndex >= 0;
}

bool visible(const int index) {
  if (index < 0 || index >= count()) return false;
  const Entry& entry = at(index);
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  if (entry.pluginIndex >= 0) {
    return !pluginHidden(gPlugins[static_cast<size_t>(entry.pluginIndex)].id);
  }
#endif
  return (SETTINGS.hiddenAppsMask & appBit(entry.id)) == 0;
}

bool setVisible(const int index, const bool show) {
  if (index < 0 || index >= count()) return false;
  const Entry& entry = at(index);
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  if (entry.pluginIndex >= 0) {
    const std::string& id = gPlugins[static_cast<size_t>(entry.pluginIndex)].id;
    auto it = std::find(gHiddenPlugins.begin(), gHiddenPlugins.end(), id);
    if (show) {
      if (it == gHiddenPlugins.end()) return false;
      gHiddenPlugins.erase(it);
    } else {
      if (it != gHiddenPlugins.end()) return false;
      gHiddenPlugins.push_back(id);
    }
    gHiddenDirty = true;
    flushVisibility();
    return true;
  }
#endif
  const uint32_t bit = appBit(entry.id);
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
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  ensureReady();
  const uint32_t mask = effectiveHiddenMask(hiddenMask, hasOpdsServers);
  int visibleCount = 0;
  for (const auto& entry : gEntries) {
    if (entry.pluginIndex >= 0) {
      if (!pluginHidden(gPlugins[static_cast<size_t>(entry.pluginIndex)].id)) ++visibleCount;
    } else if ((mask & appBit(entry.id)) == 0) {
      ++visibleCount;
    }
  }
  return visibleCount;
#else
  const uint32_t mask = effectiveHiddenMask(hiddenMask, hasOpdsServers);
  int visibleCount = 0;
  for (const auto& entry : kEntries) {
    if ((mask & appBit(entry.id)) == 0) ++visibleCount;
  }
  return visibleCount;
#endif
}

int visibleToAppIndex(const uint32_t hiddenMask, const bool hasOpdsServers, const int visibleIndex) {
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  ensureReady();
  const uint32_t mask = effectiveHiddenMask(hiddenMask, hasOpdsServers);
  int visible = 0;
  for (int i = 0; i < static_cast<int>(gEntries.size()); ++i) {
    const Entry& entry = gEntries[static_cast<size_t>(i)];
    const bool hidden = entry.pluginIndex >= 0 ? pluginHidden(gPlugins[static_cast<size_t>(entry.pluginIndex)].id)
                                               : (mask & appBit(entry.id)) != 0;
    if (hidden) continue;
    if (visible++ == visibleIndex) return i;
  }
  return -1;
#else
  const uint32_t mask = effectiveHiddenMask(hiddenMask, hasOpdsServers);
  int visible = 0;
  for (int i = 0; i < kCount; ++i) {
    if ((mask & appBit(kEntries[i].id)) != 0) continue;
    if (visible++ == visibleIndex) return i;
  }
  return -1;
#endif
}

bool activate(const int index) {
  if (index < 0 || index >= count()) return false;
  const Entry& entry = at(index);
#if defined(CROSSPOINT_PLUGIN_RUNTIME)
  if (entry.pluginIndex >= 0) {
    if (entry.pluginIndex >= static_cast<int>(gPlugins.size())) return false;
    activityManager.goToNativePlugin(gPlugins[static_cast<size_t>(entry.pluginIndex)].id);
    return true;
  }
#endif
  if (entry.open == nullptr) return false;
  entry.open();
  return true;
}

}  // namespace app_catalog
