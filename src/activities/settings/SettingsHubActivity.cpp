#include "SettingsHubActivity.h"

#include <BoardConfig.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "DictionaryDownloadActivity.h"
#include "FontDownloadActivity.h"
#if !defined(SIMULATOR)
#include "FontExtrasActivity.h"
#endif
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "TextSettingsActivity.h"
#include "VocabularyActivity.h"
#include "activities/plugins/PluginCatalogActivity.h"
#include "util/PluginEvents.h"
#if defined(CPYOS_PLUS_VENDOR_OTA)
#include "plus_ota_activity.h"
#include "plus_resources_activity.h"
#else
#include "OtaUpdateActivity.h"
#endif
#if defined(CROSSPOINT_PDF_XIP)
#include "PdfEngineUpdateActivity.h"
#endif
#include "components/UITheme.h"

namespace fui = freeink::ui;

void SettingsHubActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

void SettingsHubActivity::rebuildRows() {
  kinds_.clear();
  rows_.clear();

  const auto add = [this](const Kind kind, const StrId label) {
    kinds_.push_back(kind);
    fui::ListItem item;
    item.label = I18N.get(label);
    item.actionValue = static_cast<int16_t>(rows_.size());
    rows_.push_back(std::move(item));
  };

  if (page_ == Page::Fonts) {
    add(Kind::TextSettings, StrId::STR_TEXT_SETTINGS);
    add(Kind::FontDownload, StrId::STR_MANAGE_FONTS);
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR)
    // Font extensions stream vector packs from the SD card; without PSRAM the
    // C3 keeps its single resident face and this row would only lead to memory
    // pressure (the same gate the Settings list used).
    add(Kind::FontExtras, StrId::STR_MANAGE_FONT_EXTRAS);
#endif
    add(Kind::Dictionaries, StrId::STR_MANAGE_DICTIONARIES);
    add(Kind::VocabularyToggle, StrId::STR_SAVE_VOCABULARY_LOOKUPS);
    add(Kind::VocabularyHistory, StrId::STR_VIEW_VOCABULARY);
  } else if (page_ == Page::Updates) {
    add(Kind::CheckUpdates, StrId::STR_CHECK_UPDATES);
    add(Kind::SdFirmware, StrId::STR_SD_FIRMWARE_UPDATE);
#if defined(CROSSPOINT_PDF_XIP) && !defined(CROSSPOINT_PLUGIN_RUNTIME)
    // Images with the native plugin runtime keep the engine on the Plugins page.
    add(Kind::PdfEngine, StrId::STR_PDF_ENGINE_UPDATE);
#endif
#if defined(CPYOS_PLUS_VENDOR_OTA)
    add(Kind::PlusResources, StrId::STR_DEVICE_RESOURCES);
#endif
  } else {
    // The switch and the catalog are one page: the setting is useless without
    // its catalog, and the catalog is empty while disabled.
    add(Kind::WebPluginToggle, StrId::STR_WEB_PLUGINS);
    add(Kind::WebPluginCatalog, StrId::STR_PLUGIN_DIRECTORY);
  }
}

void SettingsHubActivity::releaseRows() {
  // Children can be the largest residents in the app (font packs, the 1.9 MB
  // engine image); hand their heap back before opening one.
  std::vector<fui::ListItem>().swap(rows_);
}

const char* SettingsHubActivity::headerTitle() const {
  // tr() is a macro over a literal id, so the choice has to be made first.
  StrId title = StrId::STR_UPDATE;
  if (page_ == Page::Fonts) {
    title = StrId::STR_SET_GROUP_FONTS_DICTS;
  } else if (page_ == Page::WebPlugins) {
    title = StrId::STR_WEB_PLUGINS;
  }
  return I18N.get(title);
}

void SettingsHubActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (size_t i = 0; i < rows_.size(); ++i) {
    rows_[i].toggle = false;
    if (kinds_[i] == Kind::VocabularyToggle) {
      GUI.setCheckboxRow(rows_[i], SETTINGS.vocabularyHistoryEnabled != 0);
    } else if (kinds_[i] == Kind::WebPluginToggle) {
      GUI.setCheckboxRow(rows_[i], SETTINGS.pluginsEnabled != 0);
    }
  }

  fui::ListProps props;
  props.items = rows_.data();
  props.count = static_cast<uint16_t>(rows_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = UITheme::getInstance().hasMainTabs() ? screen.theme().bodyText : screen.theme().smallText;
  props.labelText.maxLines = 2;  // also the explicitly-set marker, see SettingsActivity
  syncListViewport(screen, props);
  screen.list(props);
}

void SettingsHubActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(kinds_.size())) return;
  nav.selected = index;
  // The row either opens a page or flips its switch; a lingering tap flash
  // would gray an unrelated element on the repaint.
  app.clearTapFlash();

  if (kinds_[static_cast<size_t>(index)] == Kind::VocabularyToggle) {
    SETTINGS.vocabularyHistoryEnabled = SETTINGS.vocabularyHistoryEnabled ? 0 : 1;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }
  if (kinds_[static_cast<size_t>(index)] == Kind::WebPluginToggle) {
    SETTINGS.pluginsEnabled = SETTINGS.pluginsEnabled ? 0 : 1;
    SETTINGS.saveToFile();
    // Applies to discovery and event delivery now, not after a reboot.
    pluginevents::refreshSubscriptions();
    requestUpdate();
    return;
  }
  openChild(index);
}

void SettingsHubActivity::openChild(const int index) {
  releaseRows();
  const auto after = [this](const ActivityResult&) {
    rebuildRows();
    requestUpdate();
  };

  bool started = false;
  switch (kinds_[static_cast<size_t>(index)]) {
    case Kind::TextSettings:
      started = startActivityForResultWith<TextSettingsActivity>(
          [this](const ActivityResult&) {
            // The SD font system may have swapped faces while the text page was
            // open; make sure the renderer's current face is loaded again.
            {
              RenderLock lock(*this);
              sdFontSystem.ensureLoaded(renderer);
            }
            rebuildRows();
            requestUpdate();
          },
          &sdFontSystem.registry(), TextSettingsActivity::Tab::Family);
      break;
    case Kind::FontDownload:
      started = startActivityForResultWith<FontDownloadActivity>(after);
      break;
    case Kind::FontExtras:
#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR)
      started = startActivityForResultWith<FontExtrasActivity>(after);
#endif
      break;
    case Kind::Dictionaries:
      started = startActivityForResultWith<DictionaryDownloadActivity>(after);
      break;
    case Kind::VocabularyHistory:
      started = startActivityForResultWith<VocabularyActivity>(after);
      break;
    case Kind::CheckUpdates:
#if defined(CPYOS_PLUS_VENDOR_OTA)
      // The GitHub release manifests target Read Pico and must never be offered
      // on the Plus; that board uses the vendor OTA channel.
      started = startActivityForResultWith<PlusOtaActivity>(after);
#else
      started = startActivityForResultWith<OtaUpdateActivity>(after);
#endif
      break;
    case Kind::SdFirmware:
      started = startActivityForResultWith<SdFirmwareUpdateActivity>(after);
      break;
    case Kind::PdfEngine:
#if defined(CROSSPOINT_PDF_XIP) && !defined(CROSSPOINT_PLUGIN_RUNTIME)
      started = startActivityForResultWith<PdfEngineUpdateActivity>(after);
#endif
      break;
    case Kind::PlusResources:
#if defined(CPYOS_PLUS_VENDOR_OTA)
      started = startActivityForResultWith<PlusResourcesActivity>(after);
#endif
      break;
    case Kind::WebPluginCatalog:
      // rootMode=false: Back returns to this page, not Home.
      started = startActivityForResultWith<PluginCatalogActivity>(after, /*showOpds=*/false, /*rootMode=*/false);
      break;
    case Kind::VocabularyToggle:
    case Kind::WebPluginToggle:
      break;
  }

  if (!started) {
    // OOM or a row that this build does not compile: keep the page usable.
    LOG_ERR("SET", "hub child failed to start (page=%u row=%d)", static_cast<unsigned>(page_), index);
    rebuildRows();
    requestUpdate();
  }
}