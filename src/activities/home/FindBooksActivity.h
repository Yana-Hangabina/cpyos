#pragma once

#include <I18n.h>

#include "activities/Activity.h"

// Third main tab: a source directory, not a merged ranking (refreshed UI frame 08).
// A top row opens the way books arrive on the card (the web upload page), then
// one card per source with its own status lamp and its real entry points
// (JJWXC shows shelf / discover / search / login inline).
//
// Nothing here invents a count: this screen never reads the shelf, so the cards
// say "会话有效" / "未登录" (or the local/OPDS description) and the lamp only
// reports what the firmware can verify locally — a stored session file, never
// "the server still accepts it".
class FindBooksActivity final : public Activity {
 public:
  FindBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FindBooks", renderer, mappedInput) {}

  MainTab mainTab() const override { return MainTab::Apps; }
  // Pure 1-bit cards that repaint in full; moving the ring changes only the two
  // outline rings, so the diff stays a small band.
  bool wantsBandUpdates() const override { return true; }
  void debugMoveRing(int delta) override;
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Every destination the page can reach, in one enum: the row rects recorded
  // by render() carry these instead of indices into a table that the
  // compile-time source gating can shorten.
  enum class Action : uint8_t {
    None,
    Transfer,       // top row: the web upload page
    JjwxcShelf,     // 晋江 app, landing on its shelf
    JjwxcDiscover,  // 晋江 app, landing on discovery
    JjwxcSearch,    // 晋江 app, landing on search
    JjwxcAccount,   // 晋江 app, landing on login/account
    WeRead,         // 微信读书 app, shelf
    WeReadAccount,  // 微信读书 app, manage/account tab (QR page when signed out)
    LocalBrowser,   // file browser at the card root
    LocalTransfer,  // same web upload page, as the local card's second pill
    Opds,           // OPDS server list / browser
  };

  static constexpr int kMaxRows = 5;
  static constexpr int kMaxPills = 4;

  int rowCount() const;
  static Action primaryFor(int row);
  static Action secondaryFor(int row);
  static Action tertiaryFor(int row);
  static Action quaternaryFor(int row);
  StrId labelFor(Action action) const;

  // Reads the local session state once per entry: the cards' lamps and status
  // lines are drawn from this, never from a network probe.
  void refreshSourceState();
  void openAction(Action action);
  void openJjwxc(Action action);

  int selected_ = 0;
  bool jjwxcSession_ = false;
  bool wereadSession_ = false;

  // Hit rects recorded while drawing (render runs on the render task; loop()
  // is the only reader). Width 0 = no such control this frame.
  Rect rowRects_[kMaxRows] = {};
  Rect pillRects_[kMaxRows][kMaxPills] = {};
  Action rowActions_[kMaxRows] = {};
  Action pillActions_[kMaxRows][kMaxPills] = {};
};
