#include "ReaderRecoveryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointState.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReaderRecoveryActivity::onEnter() {
  Activity::onEnter();

  LOG_INF("RECOVERY", "previous book open was interrupted (%u attempt(s)); offering recovery for '%s'",
          static_cast<unsigned>(APP_STATE.readerActivityLoadCount), APP_STATE.openEpubPath.c_str());

  // Skip on the left, retry on the right and selected by default: the user was
  // reading this book, so the likely intent is to get back into it.
  const char* options[] = {I18N.get(StrId::STR_READER_RECOVERY_SKIP), I18N.get(StrId::STR_RETRY)};
  popup.show(I18N.get(StrId::STR_READER_RECOVERY_TITLE), APP_STATE.openEpubPath.c_str(), options, 2, 1,
             [this](int index) {
               if (index == 1) {
                 retryBook();
               } else {
                 skipBook();
               }
             });
  requestUpdate(true);
}

void ReaderRecoveryActivity::loop() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Back (or a tap outside the card) is the safe choice: drop the resume point.
  skipBook();
}

void ReaderRecoveryActivity::render(RenderLock&&) {
  if (popup.processRender(renderer, mappedInput)) return;
  // The popup draws itself over whatever the framebuffer holds; at boot that is
  // the splash or a cleared buffer, so paint a page first instead of leaving a
  // stale frame behind the card.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_READER_RECOVERY_TITLE));
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  auto bodyLines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_READER_RECOVERY_BODY),
                                        pageWidth - 2 * metrics.contentSidePadding, 3);
  for (const auto& line : bodyLines) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.c_str());
    y += lineHeight;
  }
  renderer.displayBuffer();
}

void ReaderRecoveryActivity::retryBook() {
  const std::string path = APP_STATE.openEpubPath;
  if (path.empty()) {
    skipBook();
    return;
  }
  // Count this attempt too: if it dies the same way, the next boot comes back to
  // this page rather than looping. ReaderActivity clears the counter on success.
  APP_STATE.readerActivityLoadCount++;
  APP_STATE.saveToFile();
  LOG_INF("RECOVERY", "retrying '%s' (attempt %u)", path.c_str(),
          static_cast<unsigned>(APP_STATE.readerActivityLoadCount));
  activityManager.goToReader(path);
}

void ReaderRecoveryActivity::skipBook() {
  LOG_INF("RECOVERY", "dropping the resume point for '%s'", APP_STATE.openEpubPath.c_str());
  APP_STATE.openEpubPath.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  activityManager.goHome();
}
