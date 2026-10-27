#include "ConfirmationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const BodyPlacement bodyPlacement)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body), bodyPlacement(bodyPlacement) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  // The dialog card carries both lines now (design frame 18): the heading as its
  // title and the body (the subject being confirmed -- a book title, a cache name)
  // as the wrapped headline beneath it. There is no page text and no clearScreen, so
  // the popup overlays the retained parent screen: the user keeps the context they
  // confirmed from, and the dialog costs one differential push instead of a white
  // wipe followed by a full parent repaint. bodyPlacement is legacy -- with the body
  // rendered inside the card, the heading is always the title; fall back to the body
  // only when a caller passed no heading.
  const char* options[] = {I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM)};
  const char* title = heading.empty() ? body.c_str() : heading.c_str();
  const char* headlineText = heading.empty() ? nullptr : body.c_str();
  confirmPopup.show(title, headlineText, options, 2, 0, [this](int idx) {
    ActivityResult res;
    res.isCancelled = (idx != 1);
    setResult(std::move(res));
    finish();
  });

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  (void)lock;
  // Overlay: OptionPopup paints its scrim + card over the retained parent frame (no
  // clearScreen), so the screen the user confirmed from stays visible behind the
  // dialog and dismissal restores it with a single push. processRender owns the
  // push while the popup is up; the fallback covers the frame it opens in.
  if (confirmPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
