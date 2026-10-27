#include "CrashActivity.h"

#include <GfxRenderer.h>
#include <HalSystem.h>
#include <I18n.h>

#include "BootTimings.h"
#include "components/UITheme.h"
#include "fontIds.h"

void CrashActivity::onEnter() {
  Activity::onEnter();

  panicMessage = HalSystem::getPanicInfo(false);
  if (panicMessage.empty()) {
    panicMessage = tr(STR_CRASH_NO_REASON);
  }

  // docs/07 D2: when the boot that panicked never reached its first frame, the
  // record it left behind names the stage it died in - the one fact the panic
  // reason and the stack dump cannot give.
  char trail[48];
  if (boottimings::incompletePrevious(trail, sizeof(trail)) > 0) {
    bootTrailMessage = std::string(tr(STR_CRASH_BOOT_STAGE)) + " " + trail;
  }

  requestUpdateAndWait();
}

void CrashActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void CrashActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // Crash report is a dead end, not a pushed screen: no back button.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CRASH_TITLE), nullptr,
                 false);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto descLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_CRASH_DESCRIPTION), contentWidth, 10);
  for (const auto& line : descLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_CRASH_REASON));
  y += lineHeight + metrics.verticalSpacing;

  auto panicLines = renderer.wrappedText(UI_10_FONT_ID, panicMessage.c_str(), contentWidth, 5);
  for (const auto& line : panicLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  if (!bootTrailMessage.empty()) {
    y += metrics.verticalSpacing;
    auto trailLines = renderer.wrappedText(UI_10_FONT_ID, bootTrailMessage.c_str(), contentWidth, 3);
    for (const auto& line : trailLines) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineHeight;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
