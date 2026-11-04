#pragma once

#include <functional>
#include <utility>

#include "components/OptionPopup.h"

// A confirmation that stays inside its host activity.
//
// Every confirmation used to be a pushed ConfirmationActivity: the dialog
// looked the same, but opening one swapped the whole activity (parent loop
// paused, an extra frame on open and a full parent repaint on close). This
// wraps OptionPopup instead, so the host paints it in its own frame and keeps
// running. The safe choice (Cancel) is the default focus, matching the old
// dialog; the caller decides whether a destructive action needs a second ask.
//
// Usage (three lines per host):
//   loop():   if (confirm_.handleInput(input, [this] { requestUpdate(); })) return;
//   render(): if (confirm_.active()) { confirm_.paint(renderer); }   // + present
//   ask:      confirm_.ask(title, subject, tr(STR_CONFIRM), [this] { ... });
class InlineConfirm {
 public:
  void ask(const char* title, const char* subject, const char* confirmLabel, std::function<void()> onConfirm,
           std::function<void()> onCancel = nullptr) {
    onConfirm_ = std::move(onConfirm);
    onCancel_ = std::move(onCancel);
    const char* options[2] = {I18N.get(StrId::STR_CANCEL), confirmLabel != nullptr ? confirmLabel : tr(STR_CONFIRM)};
    // showMessage() puts the subject in the dialog's wrapped body band; the
    // heading stays the caption. Default index 0 = Cancel.
    dialog_.showMessage(title, subject != nullptr ? subject : "", options, 2, 0, [this](const int index) {
      auto confirmed = std::move(onConfirm_);
      auto cancelled = std::move(onCancel_);
      onConfirm_ = nullptr;
      onCancel_ = nullptr;
      if (index == 1) {
        if (confirmed) confirmed();
      } else if (cancelled) {
        cancelled();
      }
    });
  }

  bool active() const { return dialog_.isActive(); }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    return dialog_.handleInput(input, requestUpdate);
  }

  // Paint only (the host owns presentation).
  void paint(const GfxRenderer& renderer) const { dialog_.render(renderer); }

  // Paint + present, for hosts that are not a UiListActivity.
  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    return dialog_.processRender(renderer, input);
  }

  // Drop the dialog without firing (the surface under it is going away).
  void dismiss() { dialog_.dismiss(); }

 private:
  OptionPopup dialog_;
  std::function<void()> onConfirm_;
  std::function<void()> onCancel_;
};
