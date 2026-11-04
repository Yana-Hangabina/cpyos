#pragma once

#include <cstdint>

#include <I18n.h>

#include "AppVisibility.h"
#include "components/icons/inx_apps.h"

// The single list of launched sub-apps. MyActivity's tool drawer renders it and
// the app-visibility settings page edits it; there is no second launcher.
namespace app_catalog {

struct Entry {
  appVisibility::AppId id;
  StrId titleId;
  UIIcon icon;
  void (*open)();
};

int count();
const Entry& at(int index);
StrId titleId(int index);
bool visible(int index);
bool setVisible(int index, bool visible);
uint32_t effectiveHiddenMask(uint32_t hiddenMask, bool hasOpdsServers);
int visibleCount(uint32_t hiddenMask, bool hasOpdsServers);
int visibleToAppIndex(uint32_t hiddenMask, bool hasOpdsServers, int visibleIndex);

}  // namespace app_catalog
