#pragma once

#include <cstdint>

#include <I18n.h>

#include "AppVisibility.h"
#include "components/icons/inx_apps.h"

// The single list of launched sub-apps. MyActivity's tool drawer renders it and
// the app-visibility settings page edits it; there is no second launcher.
//
// On plugin-runtime builds the catalog also contains one entry per installed
// native plugin. Those entries are rebuilt from the SD card; their visibility
// is stored by plugin id (not by a fixed bit), so installing another plugin or
// uninstalling one never moves another plugin's hidden state.
namespace app_catalog {

struct Entry {
  appVisibility::AppId id;
  StrId titleId;
  UIIcon icon;
  void (*open)();
  // >= 0 marks a dynamic plugin row. Built-in rows keep the default.
  int16_t pluginIndex = -1;
};

int count();
const Entry& at(int index);
// Localized built-in title or the plugin's manifest name. The pointer stays
// valid until the next refreshPlugins().
const char* title(int index);
StrId titleId(int index);
bool isPlugin(int index);
bool visible(int index);
bool setVisible(int index, bool visible);
uint32_t effectiveHiddenMask(uint32_t hiddenMask, bool hasOpdsServers);
int visibleCount(uint32_t hiddenMask, bool hasOpdsServers);
int visibleToAppIndex(uint32_t hiddenMask, bool hasOpdsServers, int visibleIndex);

// Launch a catalog row. Built-ins call their own opener; plugin rows resolve
// the package by id so a stale row cannot launch the wrong ELF.
bool activate(int index);

#if defined(CROSSPOINT_PLUGIN_RUNTIME)
// Re-read the installed plugin packages. Call on entering the tool drawer or
// the visibility page, and after the plugin manager installs/removes a package.
void refreshPlugins();
// Mark the plugin list stale; the next refresh rescans the SD card.
void invalidatePlugins();
bool pluginsDirty();
// Persist the per-plugin hidden ids (deferred until the visibility page exits).
void flushVisibility();
// Localized name for a known built-in game package, or nullptr for an unknown
// package (callers fall back to the manifest name).
const char* pluginTitle(const char* id);
#endif

}  // namespace app_catalog
