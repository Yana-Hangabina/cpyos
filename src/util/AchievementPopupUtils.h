#pragma once

#include <Arduino.h>
#include "components/Toast.h"
#include <GfxRenderer.h>

#include <string>

#include "AchievementsStore.h"
#include "fontIds.h"
#include "components/UITheme.h"

inline bool showPendingAchievementPopups(const GfxRenderer& renderer, const unsigned long delayMs = 700) {
  if (!SETTINGS.achievementPopups) {
    ACHIEVEMENTS.clearPendingUnlocks();
    return false;
  }

  bool showedAny = false;
  while (ACHIEVEMENTS.hasPendingUnlocks()) {
    const std::string message = ACHIEVEMENTS.popNextPopupMessage();
    if (message.empty()) {
      break;
    }
    Toast toast;
    toast.show(message.c_str(), millis(), static_cast<uint32_t>(delayMs));
    toast.paint(renderer, SMALL_FONT_ID, 24);
    renderer.presentOverlay();
    delay(delayMs);
    showedAny = true;
  }
  return showedAny;
}
