#include "AboutActivity.h"
#include <I18n.h>

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <esp_mac.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_DEVICE = 0,
  ITEM_FIRMWARE,
  ITEM_CHIP,
  ITEM_FLASH,
  ITEM_SD,
  ITEM_DISPLAY,
  ITEM_RESOLUTION,
  ITEM_TOUCH,
  ITEM_FRONTLIGHT,
  ITEM_RTC,
  ITEM_IMU,
  ITEM_MAC,
};

// Translated labels (user request, 2026-10-16): the page is user-facing on a
// Chinese device and read as a wall of English. The upstream note asked for
// fixed English labels so support can read screenshots; the values (device
// data, part numbers) stay identical on every unit, only the labels localise.
static constexpr StrId kAboutLabelKeys[AboutActivity::ITEM_COUNT] = {
    StrId::STR_ABOUT_DEVICE,     StrId::STR_ABOUT_FIRMWARE,   StrId::STR_ABOUT_CHIP,
    StrId::STR_ABOUT_FLASH,      StrId::STR_ABOUT_SD,         StrId::STR_ABOUT_DISPLAY,
    StrId::STR_ABOUT_RESOLUTION,
    StrId::STR_ABOUT_TOUCH,      StrId::STR_ABOUT_FRONTLIGHT, StrId::STR_ABOUT_RTC,
    StrId::STR_ABOUT_IMU,        StrId::STR_ABOUT_MAC,
};

// Chip part numbers, not user prose — deliberately untranslated.
const char* displayControllerName(const BoardConfig::DisplayController c) {
  switch (c) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LovyanGFX EPD";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279C:
      return "UC8279C";
#if defined(CROSSPOINT_PLUS_PORT)
    // The shared freeink-sdk has no epdiy controller; the Metalio Plus port
    // supplies its own compat BoardConfig with this enum value.
    case BoardConfig::DisplayController::Epdiy:
      return "epdiy EPD";
#endif
  }
  return "?";
}

const char* touchControllerName(const BoardConfig::TouchController c) {
  switch (c) {
    case BoardConfig::TouchController::None:
      return nullptr;
    case BoardConfig::TouchController::Chsc6x:
      return "CHSC6X";
    case BoardConfig::TouchController::Gt911:
      return "GT911";
    case BoardConfig::TouchController::Ft5x06:
      return "FT5x06";
    case BoardConfig::TouchController::Ft6336u:
      return "FT6336U";
    case BoardConfig::TouchController::Gslx680:
      return "GSLX680";
    case BoardConfig::TouchController::Cst816s:
      return "CST816S";
    case BoardConfig::TouchController::Cst836u:
      return "CST836U";
    // Tma525c exists in newer freeink-sdk revisions; the pinned submodule in
    // this tree predates it, so the switch falls through to the "?" default.
  }
  return "?";
}
}  // namespace

AboutActivity::AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("About", renderer, mappedInput) {}

const char* AboutActivity::headerTitle() const { return "About"; }

void AboutActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = I18N.get(kAboutLabelKeys[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }

  char buf[32];
  // Hardware and firmware information is fixed after boot; fill it once here.
  // BoardConfig::ACTIVE reflects RUNTIME detection: selectDevice() picked the
  // profile and applyXteinkDisplayController() may have promoted the display
  // controller to the panel actually found on the bus.
  rowValues_[ITEM_DEVICE] = BoardConfig::ACTIVE.name;
  rowValues_[ITEM_FIRMWARE] = CROSSPOINT_VERSION;
  snprintf(buf, sizeof(buf), "%s rev %u", ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()));
  rowValues_[ITEM_CHIP] = buf;
  snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(ESP.getFlashChipSize() / (1024u * 1024u)));
  rowValues_[ITEM_FLASH] = buf;
  uint64_t sdTotal = 0, sdFree = 0;
  if (Storage.getSpace(sdTotal, sdFree) && sdTotal > 0) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    if (sdTotal >= static_cast<uint64_t>(kGiB)) {
      snprintf(buf, sizeof(buf), "%.1f / %.1f GB", (sdTotal - sdFree) / kGiB, sdTotal / kGiB);
    } else {
      snprintf(buf, sizeof(buf), "%.0f / %.0f MB", (sdTotal - sdFree) / (1024.0 * 1024.0),
               sdTotal / (1024.0 * 1024.0));
    }
    rowValues_[ITEM_SD] = buf;
  } else {
    rowValues_[ITEM_SD] = "?";
  }
  if (const char* runtime = display.getControllerName()) {
    rowValues_[ITEM_DISPLAY] = runtime;
  } else {
    rowValues_[ITEM_DISPLAY] = displayControllerName(BoardConfig::ACTIVE.displayController);
  }
  snprintf(buf, sizeof(buf), "%ux%u", static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
           static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight));
  rowValues_[ITEM_RESOLUTION] = buf;
  const char* touch = touchControllerName(BoardConfig::ACTIVE.touch.controller);
  rowValues_[ITEM_TOUCH] = touch ? touch : "No";
  rowValues_[ITEM_FRONTLIGHT] = Frontlight.present() ? "Yes" : "No";
  rowValues_[ITEM_RTC] = halClock.isAvailable() ? "Yes" : "No";
  rowValues_[ITEM_IMU] = halTiltSensor.isAvailable() ? "Yes" : "No";
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  rowValues_[ITEM_MAC] = buf;
}

void AboutActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
