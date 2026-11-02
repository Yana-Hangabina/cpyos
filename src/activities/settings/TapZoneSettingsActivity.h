#pragma once

#include <array>

#include "TapZones.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Editor for the reader's tap-zone grid: the 3x3 main cells plus six small
// corner/edge zones. The user picks LEGACY (the stock outer-thirds layout) or
// the custom grid, toggles between editing short-press and long-press actions,
// and assigns an action to a cell through the shared OptionPopup.
//
// The preview band draws exactly the rectangles the reader hit-tests with the
// same tapzones::Grid geometry, so a cell shown in the editor is the cell the
// reader's tap lands on.
class TapZoneSettingsActivity final : public Activity {
 public:
  explicit TapZoneSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TapZones", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Layout {
    Rect header;
    Rect legacyPill;
    Rect customPill;
    Rect shortPill;
    Rect longPill;
    Rect resetButton;
    Rect gridArea;
  };

  Layout layout() const;
  void moveSelection(int dx, int dy);
  void openPopup(uint8_t zone);
  void rebuildPopupOptions();
  void applyAction(int index);
  void setMode(uint8_t mode);
  void resetZones();
  void drawPill(const Rect& rect, const char* label, bool selected) const;
  void drawZone(const Rect& rect, uint8_t action, bool selected, bool mini, bool longLabel) const;
  const char* shortActionLabel(uint8_t action) const;
  const char* longActionLabel(uint8_t action) const;
  StrId shortActionNameId(uint8_t action) const;
  StrId longActionNameId(uint8_t action) const;

  uint8_t selected_ = 4;  // centre cell
  bool longLayer_ = false;
  uint8_t popupZone_ = 4;
  // The release that closes the picker can still be reported as a tap on the
  // next loop pass; ignore touch briefly so it cannot reopen the picker (or
  // switch a pill) it just dismissed.
  uint32_t ignoreTapUntilMs_ = 0;
  OptionPopup popup_{false};
  // Points at I18n's stable strings; OptionPopup's const char* overload keeps
  // the array without copying.
  std::vector<const char*> popupOptions_;
  char popupTitle_[40] = {};
};
