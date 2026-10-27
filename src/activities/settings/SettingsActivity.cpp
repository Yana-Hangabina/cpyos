#include "SettingsActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#if (FREEINK_DEVICE_MURPHY_M4 || FREEINK_CAP_HAPTIC) && !defined(SIMULATOR)
#include <HalGPIO.h>
#endif
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "AboutActivity.h"
#include "AppVisibilitySettingsActivity.h"
#include "BluetoothSettingsActivity.h"
#include "ButtonRemapActivity.h"
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
#include "BuzzerFeedback.h"
#endif
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "CrossPointSettings.h"
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
#include "components/icons/settings_icons.h"
#endif
#include "DictionaryDownloadActivity.h"
#include "FontDownloadActivity.h"
#include "HomeButtonSettingsActivity.h"
#include "InxItemLayout.h"
#include "KOReaderSettingsActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "ReadingStatsSettingsActivity.h"
#include "SdCardFontSystem.h"
#include "ScreenRepairActivity.h"
#include "UiCjkFallbacks.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/plugins/PluginCatalogActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/ReadingBackground.h"
#include "util/SystemSettingsReset.h"

namespace fui = freeink::ui;

namespace {
// Design frame 06: the group header (label + double rule) between the
// capsule tabs and the card. drawChrome and the list spacer must agree.
constexpr int16_t kCpyGroupHeaderH = 44;
constexpr int16_t kCpyGroupSectionGap = 12;
constexpr StrId OK_OPTION[] = {StrId::STR_OK_BUTTON};
constexpr uint64_t BYTES_PER_TENTH_GB = 100000000ULL;

enum class AboutRow : uint8_t {
  FirmwareName,
  FirmwareVersion,
  DeviceModel,
  ChipModel,
  ChipTemperature,
  WifiMacAddress,
  Uptime,
  HeapFreeTotal,
  LargestHeapBlock,
  PsramHeapFreeTotal,
  SdUsedTotal,
  Count,
};

enum class StorageLoadState : uint8_t { Loading, Available, Unavailable };

// Design frame 06 groups: each chip is a top-level section, and the rows
// inside it are split into named sub-groups instead of repeating the chip's
// own name. rank orders the groups inside a category; settings that do not
// match any rule fall into "Other" so nothing can drop out of the list.
struct SettingsGroup {
  StrId label;
  uint8_t rank;
};

SettingsGroup groupForSetting(const SettingInfo& setting) {
  using CS = CrossPointSettings;
  const auto ptr = setting.valuePtr;

  switch (setting.action) {
    case SettingAction::RemapFrontButtons:
    case SettingAction::HomeButton:
      return {StrId::STR_SET_GROUP_KEYS, 0};
    case SettingAction::TextSettings:
    case SettingAction::DownloadFonts:
    case SettingAction::ManageDictionaries:
      return {StrId::STR_SET_GROUP_FONTS_DICTS, 4};
    case SettingAction::CustomiseStatusBar:
    case SettingAction::ReadingStatsSettings:
      return {StrId::STR_SET_GROUP_STATUS_STATS, 5};
    case SettingAction::Network:
    case SettingAction::KOReaderSync:
    case SettingAction::OPDSBrowser:
      return {StrId::STR_SET_GROUP_NETWORK_TRANSFER, 1};
    case SettingAction::ClockSettings:
    case SettingAction::Language:
      return {StrId::STR_SET_GROUP_TIME_LANGUAGE, 4};
    case SettingAction::AppVisibility:
    case SettingAction::Apps:
    case SettingAction::ClearCache:
    case SettingAction::RestoreSystemSettings:
    case SettingAction::CheckForUpdates:
    case SettingAction::SdFirmwareUpdate:
    case SettingAction::ScreenRepair:
    case SettingAction::Plugins:
    case SettingAction::KeyboardLayouts:
      return {StrId::STR_SET_GROUP_APPS_MAINTENANCE, 3};
    case SettingAction::About:
      return {StrId::STR_ABOUT, 5};
    case SettingAction::Bluetooth:
      return {StrId::STR_SET_GROUP_OTHER, 2};
    case SettingAction::None:
      break;
  }

  // Display
  if (ptr == &CS::sleepScreen || ptr == &CS::sleepScreenCoverMode || ptr == &CS::sleepScreenCoverFilter ||
      ptr == &CS::quickResumeSleepScreen || ptr == &CS::standbyShortcutEnabled) {
    return {StrId::STR_SET_GROUP_SLEEP_STANDBY, 0};
  }
  if (ptr == &CS::hideBatteryPercentage || ptr == &CS::refreshFrequency || ptr == &CS::fadingFix ||
      ptr == &CS::showButtonHints || ptr == &CS::frontlightRestoreOnWake || ptr == &CS::screenInverted) {
    return {StrId::STR_SET_GROUP_SCREEN_REFRESH, 1};
  }
  if (ptr == &CS::inxLibraryLayout || ptr == &CS::inxAppsLayout || ptr == &CS::inxTabPosition ||
      ptr == &CS::uiDensity) {
    return {StrId::STR_SET_GROUP_LAYOUT, 2};
  }

  // Reader
  if (ptr == &CS::fontFamily || ptr == &CS::fakeBold || ptr == &CS::extraParagraphSpacing ||
      ptr == &CS::firstLineIndent || ptr == &CS::paragraphIndentSpaces || ptr == &CS::textAntiAliasing ||
      ptr == &CS::hyphenationEnabled) {
    return {StrId::STR_SET_GROUP_TEXT_LAYOUT, 0};
  }
  if (ptr == &CS::focusReadingEnabled || ptr == &CS::readingBackgroundEnabled ||
      ptr == &CS::readingGuideLineEnabled || ptr == &CS::readingGuideLineStyle ||
      setting.signedValuePtr == &CS::readingGuideLineOffset) {
    return {StrId::STR_SET_GROUP_READING_AIDS, 1};
  }
  if (ptr == &CS::pageTurnEffect || ptr == &CS::fastPageTurn || ptr == &CS::pageTurnDirection ||
      ptr == &CS::orientation || ptr == &CS::readerMenuStyle) {
    return {StrId::STR_SET_GROUP_PAGE_DIRECTION, 2};
  }
  if (ptr == &CS::imageRendering || ptr == &CS::imageScaling) {
    return {StrId::STR_SET_GROUP_IMAGES, 3};
  }

  // Controls
  if (ptr == &CS::sideButtonLayout || ptr == &CS::frontButtonFollowOrientation ||
      ptr == &CS::longPressButtonBehavior || ptr == &CS::longPressMenuFunction || ptr == &CS::shortPwrBtn ||
      ptr == &CS::pwrBtnFootnoteBack || ptr == &CS::doubleClickPwrLight) {
    return {StrId::STR_SET_GROUP_KEYS, 0};
  }
  if (ptr == &CS::touchReaderControls || ptr == &CS::pageTurnGesture || ptr == &CS::previousPageGesture ||
      ptr == &CS::showReaderMenu || ptr == &CS::backShortToFileBrowser ||
      (setting.key != nullptr &&
       (std::strcmp(setting.key, "tiltPageTurn") == 0 || std::strcmp(setting.key, "motionPageTurn") == 0))) {
    return {StrId::STR_SET_GROUP_TOUCH_GESTURES, 1};
  }

  // System
  if (ptr == &CS::sleepTimeoutMinutes
#if CROSSPOINT_CAP_SOUND_FEEDBACK
      || ptr == &CS::soundFeedbackLevel
#endif
#if FREEINK_CAP_HAPTIC
      || ptr == &CS::hapticFeedbackLevel
#endif
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
      || ptr == &CS::buzzerFeedbackSource || ptr == &CS::buzzerPitch || ptr == &CS::buzzerStrength
#endif
  ) {
    return {StrId::STR_SET_GROUP_POWER_FEEDBACK, 0};
  }
  if (ptr == &CS::showHiddenFiles || ptr == &CS::libraryUseMetadata ||
      ptr == &CS::removeReadBooksFromRecents || ptr == &CS::moveFinishedToReadFolder) {
    return {StrId::STR_SET_GROUP_STORAGE_DATA, 2};
  }

  return {StrId::STR_SET_GROUP_OTHER, 9};
}

class InxAboutActivity final : public Activity {
 public:
  InxAboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("About", renderer, mappedInput) {}

  void onEnter() override {
    Activity::onEnter();
    heapInfo = HalSystem::getHeapInfo();
    deviceName = BoardConfig::ACTIVE.name;
#ifdef SIMULATOR
    chipModel = "Simulator";
    wifiMacAvailable = HalSystem::getDeviceId(wifiMac);
    uptimeSeconds = millis() / 1000;
#else
    chipModel = HalSystem::getChipModel();
    wifiMacAvailable = HalSystem::getWifiStationMac(wifiMac);
    float temperature = 0.0f;
    temperatureAvailable = HalSystem::getChipTemperatureCelsius(temperature);
    if (temperatureAvailable) chipTemperatureCelsius = static_cast<int>(std::lround(temperature));
    uptimeSeconds = HalSystem::getUptimeSeconds();
#endif
    storageLoadState = StorageLoadState::Loading;
    requestUpdateAndWait();
    storageLoadState =
        Storage.getSpace(sdTotalBytes, sdFreeBytes) ? StorageLoadState::Available : StorageLoadState::Unavailable;
    requestUpdate();
  }

  void loop() override {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int pageItems = GUI.getListPageItems(contentRect().height, false);
    if (pageItems <= 0 || rowCount() <= pageItems) return;
    int nextPageStart = pageStart;
    const auto swipe = mappedInput.wasSwipe();
    if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) || swipe == MappedInputManager::SwipeDir::Up) {
      nextPageStart = ButtonNavigator::nextPageIndex(pageStart, rowCount(), pageItems);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
               swipe == MappedInputManager::SwipeDir::Down) {
      nextPageStart = ButtonNavigator::previousPageIndex(pageStart, rowCount(), pageItems);
    }
    if (nextPageStart == pageStart) return;
    pageStart = nextPageStart;
    requestUpdate();
  }

  void render(RenderLock&&) override {
    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    GUI.drawHeader(renderer, Rect{safeArea.x, safeArea.y + metrics.topPadding, safeArea.width, metrics.headerHeight},
                   tr(STR_ABOUT));
    const Rect content = contentRect();
    GUI.drawList(
        renderer, content, rowCount(), pageStart,
        [this](const int index) {
          static constexpr StrId LABELS[] = {
              StrId::STR_ABOUT_FIRMWARE_NAME,
              StrId::STR_ABOUT_FIRMWARE_VERSION,
              StrId::STR_ABOUT_DEVICE_MODEL,
              StrId::STR_ABOUT_CHIP_MODEL,
              StrId::STR_ABOUT_CHIP_TEMPERATURE,
              StrId::STR_ABOUT_WIFI_MAC_ADDRESS,
              StrId::STR_ABOUT_UPTIME,
              StrId::STR_ABOUT_HEAP_FREE_TOTAL,
              StrId::STR_ABOUT_LARGEST_HEAP_BLOCK,
              StrId::STR_ABOUT_PSRAM_HEAP_FREE_TOTAL,
              StrId::STR_ABOUT_SD_USED_TOTAL,
          };
          static_assert(sizeof(LABELS) / sizeof(*LABELS) == static_cast<size_t>(AboutRow::Count));
          return std::string(I18N.get(LABELS[static_cast<size_t>(rowAt(index))]));
        },
        nullptr, nullptr, [this](const int index) { return rowValue(rowAt(index)); }, false, nullptr, false);
    const bool canPage = rowCount() > GUI.getListPageItems(content.height, false);
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), "", canPage ? tr(STR_DIR_UP) : "", canPage ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
  }

 private:
  Rect contentRect() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    return Rect{safeArea.x, safeArea.y + metrics.topPadding + metrics.headerHeight, safeArea.width,
                safeArea.height - metrics.topPadding - metrics.headerHeight};
  }

  bool hasPsram() const {
#ifdef SIMULATOR
    return false;
#else
    return heapInfo.totalPsramBytes > 0;
#endif
  }

  int rowCount() const {
    constexpr int psramRows = 1;
    return static_cast<int>(AboutRow::Count) - (hasPsram() ? 0 : psramRows);
  }

  AboutRow rowAt(const int index) const {
    if (!hasPsram() && index >= static_cast<int>(AboutRow::PsramHeapFreeTotal)) {
      return static_cast<AboutRow>(index + 1);
    }
    return static_cast<AboutRow>(index);
  }

  std::string rowValue(const AboutRow row) const {
    char value[48];
    switch (row) {
      case AboutRow::FirmwareName:
        return tr(STR_CROSSPOINT);
      case AboutRow::FirmwareVersion:
        return CROSSPOINT_VERSION;
      case AboutRow::DeviceModel:
        return deviceName ? deviceName : tr(STR_NOT_AVAILABLE);
      case AboutRow::ChipModel:
        return chipModel ? chipModel : tr(STR_NOT_AVAILABLE);
      case AboutRow::WifiMacAddress:
        if (!wifiMacAvailable) return tr(STR_NOT_AVAILABLE);
        snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X", wifiMac[0], wifiMac[1], wifiMac[2], wifiMac[3],
                 wifiMac[4], wifiMac[5]);
        return value;
      case AboutRow::ChipTemperature:
        if (!temperatureAvailable) return tr(STR_NOT_AVAILABLE);
        snprintf(value, sizeof(value), "%d", chipTemperatureCelsius);
        return value;
      case AboutRow::Uptime: {
        const uint64_t totalMinutes = uptimeSeconds / 60;
        snprintf(value, sizeof(value), "%llu:%02llu:%02llu", static_cast<unsigned long long>(totalMinutes / (24 * 60)),
                 static_cast<unsigned long long>(totalMinutes / 60 % 24),
                 static_cast<unsigned long long>(totalMinutes % 60));
        return value;
      }
      case AboutRow::HeapFreeTotal:
        snprintf(value, sizeof(value), "%lu / %lu", static_cast<unsigned long>(heapInfo.freeBytes / 1024),
                 static_cast<unsigned long>(heapInfo.totalBytes / 1024));
        return value;
      case AboutRow::LargestHeapBlock:
        snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(heapInfo.largestFreeBlockBytes / 1024));
        return value;
      case AboutRow::PsramHeapFreeTotal:
#ifdef SIMULATOR
        return tr(STR_NOT_AVAILABLE);
#else
        snprintf(value, sizeof(value), "%lu / %lu", static_cast<unsigned long>(heapInfo.freePsramBytes / 1024),
                 static_cast<unsigned long>(heapInfo.totalPsramBytes / 1024));
        return value;
#endif
      case AboutRow::SdUsedTotal: {
        switch (storageLoadState) {
          case StorageLoadState::Loading:
            return tr(STR_LOADING);
          case StorageLoadState::Unavailable:
            return tr(STR_NOT_AVAILABLE);
          case StorageLoadState::Available:
            break;
        }
        const uint64_t usedTenths = (sdTotalBytes - sdFreeBytes + BYTES_PER_TENTH_GB / 2) / BYTES_PER_TENTH_GB;
        const uint64_t totalTenths = (sdTotalBytes + BYTES_PER_TENTH_GB / 2) / BYTES_PER_TENTH_GB;
        snprintf(value, sizeof(value), "%llu.%llu / %llu.%llu", static_cast<unsigned long long>(usedTenths / 10),
                 static_cast<unsigned long long>(usedTenths % 10), static_cast<unsigned long long>(totalTenths / 10),
                 static_cast<unsigned long long>(totalTenths % 10));
        return value;
      }
      case AboutRow::Count:
        return {};
    }
    return {};
  }

  HalSystem::HeapInfo heapInfo{};
  HalSystem::DeviceId wifiMac{};
  const char* deviceName = nullptr;
  const char* chipModel = nullptr;
  uint64_t uptimeSeconds = 0;
  uint64_t sdTotalBytes = 0;
  uint64_t sdFreeBytes = 0;
  int chipTemperatureCelsius = 0;
  bool wifiMacAvailable = false;
  bool temperatureAvailable = false;
  StorageLoadState storageLoadState = StorageLoadState::Loading;
  int pageStart = 0;
};
}  // namespace

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int initialCategory,
                                   const bool subpage)
    : UiTabListActivity("Settings", renderer, mappedInput),
      initialCategory_(std::clamp(initialCategory, 0, categoryCount - 1)),
      subpage_(subpage) {}

void SettingsActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  moveRingTo(edge == MainTabContentEdge::First ? firstSelectableRing() : lastSelectableRing());
}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  std::vector<DictionaryEntry> dictionaries;
  DictionaryRegistry::discover(dictionaries);

  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaries)) {
    if (setting.category == StrId::STR_NONE_OPT || home_button::isSetting(setting.valuePtr)) continue;
    // The shelf/apps/tab-position layouts stay live under CPY
    // (LibraryListActivity::usesCoverGrid and the apps/tab layouts honour
    // them); only the home-layout picker is dead -- CPY's home is a fixed
    // dashboard. The pinned settings review encodes exactly this split.
    if (setting.valuePtr == &CrossPointSettings::inxRecentLayout) {
      continue;
    }
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      // The sunlight fading fix is a grayscale-waveform compensation that does
      // not apply on the X4 Pro / X4 Classic (plain OTP waveform, same panels).
      if (setting.valuePtr == &CrossPointSettings::fadingFix && (BoardConfig::isX4Pro() || FREEINK_DEVICE_X4CLASSIC)) {
        continue;
      }
      // CPY's home is a fixed dashboard, so its recents-layout picker would be
      // a dead control there; INX keeps the pickable variants.
      if (setting.valuePtr == &CrossPointSettings::inxRecentLayout &&
          UITheme::getInstance().usesCpyLayout()) {
        continue;
      }
      // The density picker only drives the CPY row rhythm; INX has none.
      if (setting.valuePtr == &CrossPointSettings::uiDensity && !UITheme::getInstance().usesCpyLayout()) {
        continue;
      }
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings || setting.inReadingStatsSettings) continue;
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (BoardConfig::hasHomeKey() && setting.valuePtr == &CrossPointSettings::longPressMenuFunction) continue;
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      // These stay in the shared list for persistence and the web API, but the
      // device UI owns them in the Date & Time submenu.
      if (setting.valuePtr == &CrossPointSettings::clockAutoSync ||
          setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ ||
          setting.valuePtr == &CrossPointSettings::clockFormat) {
        continue;
      }
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  if (!BoardConfig::hasTouch()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  }
#if FREEINK_CAP_BLE_HID_HOST
  controlsSettings.push_back(SettingInfo::Action(StrId::STR_BLUETOOTH, SettingAction::Bluetooth));
#endif
  systemSettings.push_back(SettingInfo::Action(StrId::STR_APPS_TITLE, SettingAction::Apps));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_APP_VISIBILITY, SettingAction::AppVisibility));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_DATE_AND_TIME, SettingAction::ClockSettings));
  if (BoardConfig::hasHomeKey()) {
    controlsSettings.insert(controlsSettings.begin(),
                            SettingInfo::Action(StrId::STR_HOME_BUTTON, SettingAction::HomeButton));
  }
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(
      SettingInfo::Action(StrId::STR_RESTORE_SYSTEM_SETTINGS, SettingAction::RestoreSystemSettings));
#if FREEINK_DEVICE_MURPHY_M4 && !defined(SIMULATOR)
  systemSettings.push_back(
      SettingInfo::DynamicEnum(
          StrId::STR_M4_HARDWARE_BATCH, {StrId::STR_M4_BATCH_1, StrId::STR_M4_BATCH_2},
          [] { return gpio.murphyM4Batch() == freeink::MurphyM4Batch::First ? 0 : 1; },
          [this](const uint8_t value) {
            const auto batch = value == 0 ? freeink::MurphyM4Batch::First : freeink::MurphyM4Batch::Second;
            if (gpio.saveMurphyM4Batch(batch)) {
              silentRestart();
              return;
            }
            optionPopup.show(StrId::STR_FAILED_LOWER, OK_OPTION, static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
            requestUpdate();
          })
          .withManagedEnumPicker());
#endif
  // Keep the existing CrossMux OTA proxy flow. Build-only boards compile this
  // UI but are intentionally absent from release assets in this sync.
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SCREEN_REPAIR, SettingAction::ScreenRepair));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_PLUGINS, SettingAction::Plugins));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_ABOUT, SettingAction::About));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readerSettings.insert(readerSettings.begin() + 2,
                        SettingInfo::Action(StrId::STR_MANAGE_DICTIONARIES, SettingAction::ManageDictionaries));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_READING_STATS, SettingAction::ReadingStatsSettings));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
  rebuildRowItems();
}

void SettingsActivity::onEnter() {
  UiTabListActivity::onEnter();

  // Reset selection to first category (ring position 0, the tab bar, comes
  // from the base's per-tab nav reset)
  selectedCategoryIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();
  // The My dashboard opens a specific category; consume it once so a later
  // re-entry does not keep jumping back to it.
  if (initialCategory_ != 0) {
    selectCategory(initialCategory_);
    initialCategory_ = 0;
  }
}

void SettingsActivity::selectCategory(const int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
  activeNav().top = 0;  // category switches start the list at the top (no per-tab memory here)
  rebuildRowItems();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for *currentSettings.
// Structural — call only when the active category or a category's setting
// list changes, never from buildScreen(), which only refreshes rowValues_
// content and rowItems_[].value pointers in place.
void SettingsActivity::rebuildRowItems() {
  const auto& settings = *currentSettings;
  rowValues_.assign(settings.size(), std::string());
  rowItems_.clear();
  rowSettingIndex_.clear();
  rowItems_.reserve(settings.size() + 6);
  rowSettingIndex_.reserve(settings.size() + 6);

  // Design frame 06: a section header per logical group, not one header that
  // just repeats the chip above it. Grouping is applied here (display order
  // only) so the shared setting list and its persistence order stay intact.
  std::vector<uint16_t> order(settings.size());
  for (uint16_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&settings](const uint16_t left, const uint16_t right) {
    return groupForSetting(settings[left]).rank < groupForSetting(settings[right]).rank;
  });

  const bool cpyIcons = UITheme::getInstance().usesCpyLayout();
  StrId currentGroup = StrId::STR_NONE_OPT;
  for (const uint16_t index : order) {
    const SettingInfo& setting = settings[index];
    const SettingsGroup group = groupForSetting(setting);
    if (group.label != currentGroup) {
      fui::ListItem header;
      header.label = I18N.get(group.label);
      header.isHeader = true;
      header.actionValue = -1;
      rowItems_.push_back(header);
      rowSettingIndex_.push_back(-1);
      currentGroup = group.label;
    }

    fui::ListItem item;
    item.label = I18N.get(setting.nameId);
    // actionValue is this row's index in rowItems_ (FreeInkUI reports it on a
    // tap; UiTabListActivity turns it into ring = row + 1), NOT the setting
    // index: group headers inserted above make the two diverge.
    const auto row = static_cast<int16_t>(rowItems_.size());
    item.actionValue = row;
    // Design frame 06: every row carries a leading glyph; rows without a
    // sensible match stay icon-less rather than wear a misleading one. Gated
    // to the high-density profile so C3 does not pay the icon table's flash.
    if (cpyIcons) {
#ifdef CROSSMUX_UI_PROFILE_HIGH_DPI
      if (const freeink::Icon* icon = settings_icons::forSetting(setting.nameId)) {
        item.icon = fui::bitmapFromIcon(*icon);
      }
#endif
    }
    rowItems_.push_back(item);
    rowSettingIndex_.push_back(static_cast<int16_t>(index));
  }
}

int SettingsActivity::settingIndexAtRow(const int row) const {
  if (row < 0 || row >= static_cast<int>(rowSettingIndex_.size())) return -1;
  return rowSettingIndex_[static_cast<size_t>(row)];
}

int SettingsActivity::firstSelectableRing() const {
  for (size_t i = 0; i < rowSettingIndex_.size(); ++i) {
    if (rowSettingIndex_[i] >= 0) return static_cast<int>(i) + 1;
  }
  return 0;
}

int SettingsActivity::lastSelectableRing() const {
  for (int i = static_cast<int>(rowSettingIndex_.size()) - 1; i >= 0; --i) {
    if (rowSettingIndex_[static_cast<size_t>(i)] >= 0) return i + 1;
  }
  return 0;
}

int SettingsActivity::nextSelectableRing(const int from, const int direction) const {
  const int ringSize = listCount() + 1;
  if (ringSize <= 1) return from;
  int ring = from;
  for (int step = 0; step < ringSize; ++step) {
    ring = direction > 0 ? ButtonNavigator::nextIndex(ring, ringSize)
                         : ButtonNavigator::previousIndex(ring, ringSize);
    if (ring == 0 || settingIndexAtRow(ring - 1) >= 0) return ring;
  }
  return from;
}

int SettingsActivity::listCount() const { return static_cast<int>(rowItems_.size()); }

freeink::ui::ListNav& SettingsActivity::activeNav() { return UiTabListActivity::activeNav(); }


void SettingsActivity::onTabAction(const int index) {
  if (optionPopup.isActive()) return;
  selectCategory(index);
  activeNav().selected = 0;  // tab taps land with the tab bar focused
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  (void)index;  // toggleCurrentSetting reads the ring position
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  toggleCurrentSetting();
  // Tap-first: a tapped row is not a cursor position. Leaving it focused
  // (inverted) after the tap meant the row stayed black once its sub-screen or
  // popup closed, and Back then had to clear that focus before a second Back
  // left Settings. Hand the focus back to the tab band; the viewport stays put.
  if (mappedInput.hasTouch()) {
    activeNav().selected = 0;
  }
}

void SettingsActivity::onRowAction(const fui::ActionEvent& event) { UiTabListActivity::onRowAction(event); }

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}


bool SettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void SettingsActivity::navigateButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) ||
      mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    navigationStartedOnTabs = ringPos() == 0;
  }
  // Section header rows are not selectable: step over them instead of letting
  // Confirm land on a row that has no setting behind it.
  buttonNavigator.onNextPress([this] { moveRingTo(nextSelectableRing(ringPos(), 1)); });
  buttonNavigator.onPreviousPress([this] { moveRingTo(nextSelectableRing(ringPos(), -1)); });
  buttonNavigator.onNextContinuous([this] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
    } else if (ringPos() == 0 && listCount() > 0) {
      activeNav().selected = firstSelectableRing();
    }
    stepTab(1);
  });
  buttonNavigator.onPreviousContinuous([this] {
    if (navigationStartedOnTabs) {
      activeNav().selected = 0;
    } else if (ringPos() == 0 && listCount() > 0) {
      activeNav().selected = firstSelectableRing();
    }
    stepTab(-1);
  });
}

void SettingsActivity::stepTab(const int direction) {
  // Ring position 0 stays on the tab bar; a row selection collapses to the
  // new category's first row (per-tab memory is deliberately not kept here).
  const bool onTabBar = ringPos() == 0;
  selectedCategoryIndex = direction > 0 ? ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)
                                        : ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
  selectCategory(selectedCategoryIndex);
  activeNav().selected = onTabBar ? 0 : firstSelectableRing();
  requestUpdate();
}

bool SettingsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      stepTab(1);
    } else {
      toggleCurrentSetting();
      requestUpdate();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (subpage_) {
      // A child page always leaves in one press (header arrow, Back key or
      // edge gesture): it returns to My/Search, not Home.
      SETTINGS.saveToFile();
      finish();
    } else if (ringPos() > 0) {
      activeNav().selected = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return true;
  }

  return false;
}






void SettingsActivity::toggleCurrentSetting() {
  mappedInput.resetHomeButtonInput();
  const int selectedSetting = settingIndexAtRow(ringPos() - 1);
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const auto changedValuePtr = setting.valuePtr;
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
  // A plain VALUE row only steps upward on confirm; open the shared slider
  // (drag, -/+ buttons, held keys) so strength can also go down and be heard.
  if (setting.valuePtr == &CrossPointSettings::buzzerStrength) {
    openBuzzerStrengthPicker();
    return;
  }
#endif

  if (setting.valuePtr == &CrossPointSettings::readingBackgroundEnabled) {
    openReadingBackgroundMenu();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const auto enumLabels = setting.enumLabels();
    if (enumLabels.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(
          setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), currentValue,
          [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
            SETTINGS.*valuePtr = idx;
            // Kept for the case uiDensity grows past two values; the two-value
            // path above is what runs today.
            if (valuePtr == &CrossPointSettings::uiDensity) applyUiCjkFallbacks(renderer, idx != 0);
#if CROSSPOINT_CAP_BUZZER_FEEDBACK
            // Preview the sound the user just picked. The tap on the option row
            // already emitted a cue with the old values, so preview() restarts
            // instead of being swallowed by the minimum-gap window.
            if (valuePtr == &CrossPointSettings::buzzerPitch) {
              BuzzerFeedback::preview(static_cast<BuzzerFeedback::Pitch>(idx), SETTINGS.buzzerStrength);
            } else if (valuePtr == &CrossPointSettings::buzzerFeedbackSource &&
                       idx != CrossPointSettings::BUZZER_FEEDBACK_OFF) {
              BuzzerFeedback::preview(static_cast<BuzzerFeedback::Pitch>(SETTINGS.buzzerPitch),
                                      SETTINGS.buzzerStrength);
            }
#endif
#if FREEINK_CAP_HAPTIC && !defined(SIMULATOR)
            if (valuePtr == &CrossPointSettings::hapticFeedbackLevel && idx == CrossPointSettings::HAPTIC_FEEDBACK_OFF)
              gpio.stopHapticFeedback();
#endif
            syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
            SETTINGS.saveToFile();
            rebuildSettingsLists();
          });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(enumLabels.size());
    // UI density also picks the Chinese glyph size: comfortable keeps the
    // size-matched 14/16pt faces, compact uses the wide 12pt face. Re-wire the
    // renderer fallbacks before the next frame is built.
    if (setting.valuePtr == &CrossPointSettings::uiDensity) {
      applyUiCjkFallbacks(renderer, SETTINGS.uiDensity != 0);
    }
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumLabels().size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    if (totalValues > 2 || setting.managedEnumPicker) {
      const auto valueSetter = setting.valueSetter;
      const bool managedPicker = setting.managedEnumPicker;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged, managedPicker,
                       cur](const int idx) {
        if (idx == cur) return;
        valueSetter(static_cast<uint8_t>(idx));
        if (managedPicker) return;
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        const auto enumLabels = setting.enumLabels();
        optionPopup.show(setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    // Every child page returns here through ActivityManager's Pop; make sure the
    // settings list is both rebuilt (values may have changed) and repainted.
    auto resultHandler = [this](const ActivityResult&) {
      SETTINGS.saveToFile();
      rebuildSettingsLists();
      requestUpdate();
    };

    switch (setting.action) {
      case SettingAction::HomeButton: {
        // Activities must outlive this call and are owned by the activity stack.
        auto activity = makeUniqueNoThrow<HomeButtonSettingsActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SET", "OOM: Home button settings");
          return;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      case SettingAction::RemapFrontButtons:
        startActivityForResultWith<ButtonRemapActivity>(resultHandler);
        break;
      case SettingAction::Bluetooth:
#if FREEINK_CAP_BLE_HID_HOST
        releaseListsForMemoryHungryChild();
        if (!startActivityForResultWith<BluetoothSettingsActivity>(resultHandler)) {
          rebuildSettingsLists();
          requestUpdate();
        }
#endif
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResultWith<StatusBarSettingsActivity>(resultHandler);
        break;
      case SettingAction::ReadingStatsSettings:
        startActivityForResultWith<ReadingStatsSettingsActivity>(resultHandler);
        break;
      case SettingAction::Apps:
        activityManager.goToApps();
        break;
      case SettingAction::AppVisibility:
        startActivityForResultWith<AppVisibilitySettingsActivity>(resultHandler);
        break;
      case SettingAction::ClockSettings:
        if (auto activity = makeUniqueNoThrow<ClockSettingsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: ClockSettingsActivity");
        }
        break;
      case SettingAction::KOReaderSync:
        startActivityForResultWith<KOReaderSettingsActivity>(resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResultWith<OpdsServerListActivity>(resultHandler);
        break;
      case SettingAction::Network: {
        auto activity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
        if (!activity) {
          LOG_ERR("SETTINGS", "OOM: WifiSelectionActivity");
          return;
        }
        startActivityForResult(std::move(activity), [](const ActivityResult&) {
          SETTINGS.saveToFile();
          // Every other WiFi consumer hands the radio to a session it owns;
          // these rows only save credentials, so nothing here would ever
          // release the driver's heap. The scan alone brings it up, so tear
          // down whether or not the user joined a network.
          if (WiFi.getMode() == WIFI_MODE_NULL) return;
          WiFi.disconnect(false);
          delay(30);
          // Unlike the onExit() teardowns, this runs from the loop task with
          // no lock held; the restart popup paints straight to the panel.
          RenderLock lock;
          silentRestartToSettings();
        });

        break;
      }
      case SettingAction::ClearCache:
        startActivityForResultWith<ClearCacheActivity>(resultHandler);
        break;
      case SettingAction::RestoreSystemSettings:
        confirmRestoreSystemSettings();
        break;
      case SettingAction::CheckForUpdates:
        openOtaUpdate();
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResultWith<SdFirmwareUpdateActivity>(resultHandler);
        break;
      case SettingAction::ScreenRepair:
        startActivityForResultWith<ScreenRepairActivity>(resultHandler);
        break;
      case SettingAction::DownloadFonts:
        releaseListsForMemoryHungryChild();
        if (!startActivityForResultWith<FontDownloadActivity>(resultHandler)) {
          rebuildSettingsLists();
          requestUpdate();
        }
        break;
      case SettingAction::ManageDictionaries:
        startActivityForResultWith<DictionaryDownloadActivity>([this](const ActivityResult&) {
          SETTINGS.saveToFile();
          rebuildSettingsLists();
          requestUpdate();
        });
        break;
      case SettingAction::TextSettings:
        startActivityForResultWith<TextSettingsActivity>(
            [this](const ActivityResult&) {
              // TextSettingsActivity saves on each change; no save needed here.
              {
                RenderLock lock(*this);
                sdFontSystem.ensureLoaded(renderer);
              }
              rebuildSettingsLists();
              requestUpdate();
            },
            &sdFontSystem.registry(), TextSettingsActivity::Tab::Family);
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildRowItems() and don't
        // re-run on Pop (see ActivityManager::loop()), so a language switch
        // needs an explicit rebuild here rather than the generic resultHandler.
        startActivityForResultWith<LanguageSelectActivity>([this](const ActivityResult&) {
          SETTINGS.saveToFile();
          rebuildSettingsLists();
        });
        break;
      case SettingAction::About:
        startActivityForResultWith<AboutActivity>(resultHandler);
        break;
      case SettingAction::Plugins:
        startActivityForResultWith<PluginCatalogActivity>(resultHandler);
        break;
      case SettingAction::KeyboardLayouts:
        if (auto activity = makeUniqueNoThrow<KeyboardLayoutsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: KeyboardLayoutsActivity");
        }
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  rebuildSettingsLists();
  // Ring positions include the group headers; settingsCount does not. Clamping
  // to settingsCount snapped a row below a header back to an earlier setting.
  activeNav().selected = std::min(ringPos(), listCount());
}

void SettingsActivity::confirmRestoreSystemSettings() {
  const bool started = startActivityForResultWith<ConfirmationActivity>(
      [this](const ActivityResult& firstResult) {
        if (firstResult.isCancelled) return;

        const bool finalStarted = startActivityForResultWith<ConfirmationActivity>(
            [this](const ActivityResult& finalResult) {
              if (finalResult.isCancelled) return;
              if (systemSettingsReset::clearPersistedSettings()) {
                silentRestart();
                return;
              }
              optionPopup.show(StrId::STR_RESTORE_SYSTEM_SETTINGS_FAILED, OK_OPTION,
                               static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
              requestUpdate();
            },
            tr(STR_RESTORE_SYSTEM_SETTINGS), tr(STR_RESTORE_SYSTEM_SETTINGS_FINAL_WARNING));
        if (finalStarted) return;
        optionPopup.show(StrId::STR_MEMORY_ERROR, OK_OPTION, static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
        requestUpdate();
      },
      tr(STR_RESTORE_SYSTEM_SETTINGS), tr(STR_RESTORE_SYSTEM_SETTINGS_WARNING));
  if (started) return;
  optionPopup.show(StrId::STR_MEMORY_ERROR, OK_OPTION, static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
  requestUpdate();
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::releaseListsForMemoryHungryChild() {
  RenderLock lock(*this);
  closeRouting();
  // clear() keeps vector capacity; swap it out so a stacked Settings activity
  // leaves that heap to the child.
  std::vector<freeink::ui::ListItem>().swap(rowItems_);
  std::vector<std::string>().swap(rowValues_);
  std::vector<SettingInfo>().swap(displaySettings);
  std::vector<SettingInfo>().swap(readerSettings);
  std::vector<SettingInfo>().swap(controlsSettings);
  std::vector<SettingInfo>().swap(systemSettings);
  settingsCount = 0;
}

void SettingsActivity::openOtaUpdate() {
  releaseListsForMemoryHungryChild();

  if (startActivityForResultWith<OtaUpdateActivity>([this](const ActivityResult&) {
        SETTINGS.saveToFile();
        rebuildSettingsLists();
      })) {
    return;
  }

  rebuildSettingsLists();
  requestUpdate();
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResultWith<IntervalSelectionActivity>(
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      },
      "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
      CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
      StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, StrId::STR_SLEEP_NEVER);
}

#if CROSSPOINT_CAP_BUZZER_FEEDBACK
void SettingsActivity::openBuzzerStrengthPicker() {
  lastBuzzerPreviewMs_ = 0;
  startActivityForResultWith<IntervalSelectionActivity>(
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const int value = std::get<IntervalResult>(result.data).value;
          SETTINGS.buzzerStrength = static_cast<uint8_t>(std::clamp(value, 0, 100));
          SETTINGS.saveToFile();
          BuzzerFeedback::preview(static_cast<BuzzerFeedback::Pitch>(SETTINGS.buzzerPitch),
                                  SETTINGS.buzzerStrength);
        }
        requestUpdate();
      },
      "BuzzerStrength", StrId::STR_BUZZER_STRENGTH, SETTINGS.buzzerStrength, 0, 100, 1, 5, StrId::STR_NONE_OPT,
      false, StrId::STR_NONE_OPT, [this](const int value) {
        // A held key or a fast drag changes the value every few tens of ms;
        // preview at most every 120 ms so the coil does not chatter.
        const unsigned long now = millis();
        if (now - lastBuzzerPreviewMs_ < 120) return;
        lastBuzzerPreviewMs_ = now;
        BuzzerFeedback::preview(static_cast<BuzzerFeedback::Pitch>(SETTINGS.buzzerPitch),
                                static_cast<uint8_t>(std::clamp(value, 0, 100)));
      });
}
#endif

void SettingsActivity::openReadingBackgroundMenu() {
  static constexpr StrId OPTIONS[] = {StrId::STR_STATE_OFF, StrId::STR_CUSTOM_IMAGE};
  optionPopup.show(StrId::STR_READING_BACKGROUND, OPTIONS, static_cast<int>(std::size(OPTIONS)),
                   SETTINGS.readingBackgroundEnabled ? 1 : 0, [this](const int selected) {
                     if (selected == 0) {
                       SETTINGS.readingBackgroundEnabled = 0;
                       SETTINGS.saveToFile();
                       requestUpdate();
                       return;
                     }
                     openReadingBackgroundPicker();
                   });
  requestUpdate();
}

void SettingsActivity::openReadingBackgroundPicker() {
  const bool started = startActivityForResultWith<FileBrowserActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* selected = std::get_if<FilePathResult>(&result.data);
        if (!selected) {
          LOG_ERR("SET", "PNG picker returned no path");
          return;
        }

        GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        if (readingBackground::createCacheFromPng(renderer, selected->path.c_str())) {
          SETTINGS.readingBackgroundEnabled = 1;
          SETTINGS.saveToFile();
          requestUpdate();
        } else {
          optionPopup.show(StrId::STR_FAILED_LOWER, OK_OPTION, static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
        }
      },
      "/", FileBrowserActivity::Mode::PickPng);
  if (!started) {
    optionPopup.show(StrId::STR_MEMORY_ERROR, OK_OPTION, static_cast<int>(std::size(OK_OPTION)), 0, [](int) {});
    requestUpdate();
  }
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (setting.valuePtr == &CrossPointSettings::readingBackgroundEnabled) {
    return SETTINGS.readingBackgroundEnabled ? tr(STR_CUSTOM_IMAGE) : tr(STR_STATE_OFF);
  }
  if (setting.action == SettingAction::HomeButton) return tr(STR_CONFIGURE);
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    uint8_t value = SETTINGS.*(setting.valuePtr);
    if (setting.valuePtr == &CrossPointSettings::uiTheme && value >= CrossPointSettings::UI_THEME_COUNT) {
      value = CrossPointSettings::CPY;
    }
    const auto enumLabels = setting.enumLabels();
    if (value >= enumLabels.size()) return "";
    return I18N.get(enumLabels[value]);
  }
  if (setting.type == SettingType::ENUM && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    const auto enumLabels = setting.enumLabels();
    if (value < enumLabels.size()) {
      return I18N.get(enumLabels[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    return std::to_string(SETTINGS.*(setting.valuePtr));
  }
  return "";
}

void SettingsActivity::buildTabBar(UiScreen& screen, const bool) {
  // Same tab language as the My dashboard: plain text, 4 px underline on the
  // active category, no black pill. The old capsule tabs made a subgroup look
  // like a different app from the page that opened it.
  const auto& tokens = screen.theme();
  const fui::Rect band = screen.takeTop(72, 8).inset(fui::Insets{0, 40, 0, 40});
  fui::Paint ink = fui::Paint::solid(fui::Color::Black);
  int x = band.x;
  const char* labels[4] = {I18N.get(StrId::STR_CAT_DISPLAY), I18N.get(StrId::STR_CAT_READER),
                           I18N.get(StrId::STR_CAT_CONTROLS), I18N.get(StrId::STR_CAT_SYSTEM)};
  for (int i = 0; i < 4; ++i) {
    fui::TextStyle style = tokens.bodyText;
    const bool active = i == selectedCategoryIndex;
    style.bold = active;
    const int16_t textW = screen.target().measureText(style.font, labels[i], style).width;
    screen.target().text(fui::Rect{static_cast<int16_t>(x), band.y, textW, band.height}, labels[i], style);
    if (active) {
      const int16_t lineH = screen.target().lineHeight(style.font);
      const int16_t underlineY = static_cast<int16_t>(band.y + (band.height + lineH) / 2 + 4);
      screen.target().fill(fui::Rect{static_cast<int16_t>(x), underlineY, textW, 4}, ink, 2);
    }
    screen.frame().hit(fui::Rect{static_cast<int16_t>(x - 8), band.y, static_cast<int16_t>(textW + 16), band.height},
                       ACTION_TAB, static_cast<int16_t>(i), fui::InputTouch);
    x += textW + 36;
  }
  const int16_t ruleY = static_cast<int16_t>(band.bottom() - 1);
  screen.target().fill(fui::Rect{40, ruleY, static_cast<int16_t>(renderer.getScreenWidth() - 80), 1}, ink);
}

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto applyCheckbox = [](const SettingInfo& setting, fui::ListItem& item) {
    item.toggle = false;
    const auto labels = setting.enumLabels();
    const bool checkbox = setting.type == SettingType::TOGGLE ||
                          (setting.type == SettingType::ENUM && setting.enumStringValues.empty() &&
                           labels.size() == 2 && labels[0] == StrId::STR_STATE_OFF && labels[1] == StrId::STR_STATE_ON);
    if (checkbox && (setting.valuePtr || setting.valueGetter)) {
      const bool checked = setting.valuePtr ? SETTINGS.*(setting.valuePtr) != 0 : setting.valueGetter() != 0;
      GUI.setCheckboxRow(item, checked);
    }
  };
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = pageContentRect();
  // The list band spans the screen; the ROWS are inset 34 by rowInset so the
  // scroll track can live in the 32px card margin instead of on the card's
  // right border (device feedback, 2026-10-20).
  const int16_t sideMargin = UITheme::getInstance().usesCpyLayout() ? 0 : content.x;
  const int16_t rightMargin = UITheme::getInstance().usesCpyLayout()
                                  ? 0
                                  : static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), rightMargin,
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), sideMargin});


  {
    GfxRenderer::SyntheticBoldScope syntheticBold(renderer, CrossPointSettings::SYNTHETIC_BOLD_OFF);
    buildTabBar(screen, false);
  }

  // rowItems_ (label/actionValue plus group headers) was built by
  // rebuildRowItems() when the category was last selected/rebuilt; only the
  // live value text needs refreshing here. Headers have no setting behind
  // them, so every refresh goes through rowSettingIndex_.
  const auto& settings = *currentSettings;
  const bool cpyRows = UITheme::getInstance().usesCpyLayout();
  for (size_t r = 0; r < rowSettingIndex_.size(); ++r) {
    const int settingIndex = rowSettingIndex_[r];
    if (settingIndex < 0) continue;
    const SettingInfo& setting = settings[static_cast<size_t>(settingIndex)];
    rowValues_[static_cast<size_t>(settingIndex)] = settingValueText(setting);
    rowItems_[r].toggle = false;
    applyCheckbox(setting, rowItems_[r]);
    // Design frame 06: picker rows end with a chevron. The SDK ListItem has
    // no chevron slot, so the value string carries it; toggle rows keep the
    // switch and no value (setCheckboxRow nulls it — the checkbox-row pin
    // asserts exactly that). U+203A is the thin mark the mockup uses.
    if (cpyRows && !rowItems_[r].toggle) {
      std::string& value = rowValues_[static_cast<size_t>(settingIndex)];
      value += value.empty() ? "\xE2\x80\xBA" : " \xE2\x80\xBA";
    }
    const std::string& value = rowValues_[static_cast<size_t>(settingIndex)];
    rowItems_[r].value = rowItems_[r].toggle || value.empty() ? nullptr : value.c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  // maxLines=2 also marks the style explicitly set (an all-default smallText
  // fails textStyleUnset and the list would substitute bodyText back); the
  // common fits-on-one-line case takes the renderer's fast path anyway.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  // Fixed row rhythm so the per-group cards drawn in drawFooter can follow the
  // list geometry exactly; the design's rows are single-line.
  props.rowHeight = mappedInput.hasTouch() ? screen.theme().rowHeight
                                           : static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
  if (UITheme::getInstance().usesCpyLayout()) props.rowHeight = 80;
  props.rowGap = 0;
  // Section headers inside the card (design frame 06 .grp): a bold label with
  // its own underline, airier than a row.
  props.headerText = screen.theme().smallText;
  props.headerText.bold = true;
  props.headerRowHeight = kCpyGroupHeaderH;
  props.sectionGap = kCpyGroupSectionGap;
  if (UITheme::getInstance().usesCpyLayout()) {
    // Design frame 06 row metrics: the card sits at margin 32 with a 2px ring
    // and 24px row padding, so the row content runs [58, 626]. With the 34px
    // content margin above, a 24px row inset reproduces that axis exactly
    // (icons land at 58, the value/chevron flush at 626).
    props.sidePadding = 24;
    props.valueInset = 0;
    props.iconSize = 40;
    props.textGap = 18;
    // Rows sit inside the 32px card ring; the track is pushed into that outer
    // margin so it never rides the card's right border.
    props.rowInset = 34;
    props.scrollIndicatorInset = 6;
    // Fill the last incomplete row slot with a clipped preview of the next
    // setting instead of leaving an empty band above the tab bar. The preview
    // is non-interactive; scrolling reveals the full row.
    props.partialTrailingRow = true;
    props.partialTrailingMinHeight = 24;
    // Design frame 06: rows inside a group carry no dividers; the group card
    // ring is drawn under them by drawChrome.
    props.separator = fui::SeparatorStyle::None;
    // Pill switch (design frame 06 .sw): 56x32 track, fully rounded, with a
    // 24px round knob and a 2px ink outline. The SDK default is a square
    // 38x18 track with square corners — the legacy CrossMux look.
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
  syncTabListViewport(screen, props);
  screen.list(props);
}

void SettingsActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  if (subpage_) {
    // Child of My/Search: the theme draws the back circle; the category strip
    // right below already names the active group, so the header stays quiet.
    drawPageHeader(Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE));
  } else {
    drawPageHeader(Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                   CROSSPOINT_VERSION);
  }

  // The category name lives in the capsule above; the old second copy of it
  // (a group label + double rule between the capsules and the card) is gone.
  // Group names now live inside the list, one per sub-section.
}

void SettingsActivity::drawFooter() {
  // The group card ring lives here, AFTER the fui list pass: the list's row
  // background covered the ring's side strokes when drawn in drawChrome (the
  // top/bottom arcs survived because they sit outside the rows' y-span — sim
  // forensics, 2026-10-17). drawFooter runs before displayBuffer in the same
  // frame, so nothing paints over it.
  if (UITheme::getInstance().usesCpyLayout() && listRowH_ > 0 && listBottom_ > listTop_) {
    const Rect content = pageContentRect();
    constexpr int kCardMargin = 32;
    const int rx = content.x + kCardMargin;
    const int rw = content.width - kCardMargin * 2;
    const int clipTop = listTop_;
    const int clipBottom = listBottom_;
    const GfxRenderer::ClipScope clip(renderer, content.x, clipTop, content.width, clipBottom - clipTop);
    // Open ends: when a group continues past the viewport, its rounded end is
    // pushed outside the clip so only the straight sides reach the edge. A
    // closed bottom on every page made a long group look like a series of
    // separate cards (device feedback, 2026-10-20).
    const auto drawCard = [&](const int top, const int bottom, const bool roundTop, const bool roundBottom) {
      const int y0 = std::max(top, clipTop);
      const int y1 = std::min(bottom, clipBottom);
      if (y1 - y0 < 4) return;
      constexpr int kEndExtend = 24;  // > card radius; the arc lands outside the clip
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
    // Design frame 06: each group is its own rounded card. The list layout is
    // deterministic here (fixed row height, no row gap), so replay its cursor
    // from the viewport's top index and close a card at every next header.
    int cursor = clipTop;
    int groupStart = clipTop;
    bool groupHasRow = false;
    bool groupRoundTop = false;  // no visible header => the group began above
    const int topIndex = activeNav().top;
    const int count = static_cast<int>(rowItems_.size());
    for (int i = topIndex; i < count; ++i) {
      if (rowItems_[i].isHeader) {
        if (groupHasRow) drawCard(groupStart, cursor, groupRoundTop, true);
        groupHasRow = false;
        if (i != topIndex) cursor += kCpyGroupSectionGap;
        if (cursor + kCpyGroupHeaderH > clipBottom) break;
        cursor += kCpyGroupHeaderH;
        groupStart = cursor;
        groupRoundTop = true;
        continue;
      }
      if (cursor + listRowH_ > clipBottom) {
        // A partial next row is still painted (partialTrailingRow): keep the
        // group card open through it, including the first row after a header.
        if (groupHasRow || groupRoundTop) drawCard(groupStart, clipBottom, groupRoundTop, false);
        return;
      }
      if (!groupHasRow) groupHasRow = true;
      cursor += listRowH_;
    }
    if (groupHasRow) drawCard(groupStart, cursor, groupRoundTop, true);
  }

  const int ring = ringPos();
  const int settingIndex = settingIndexAtRow(ring - 1);
  const auto confirmLabel =
      (ring == 0) ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
                  : (settingIndex >= 0 &&
                             (*currentSettings)[static_cast<size_t>(settingIndex)].nameId == StrId::STR_TIME_TO_SLEEP
                         ? tr(STR_SELECT)
                         : tr(STR_TOGGLE));

  const auto labels = mappedInput.mapLabels(subpage_ ? tr(STR_BACK) : tr(STR_HOME), confirmLabel, tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
