#pragma once

#include <string>

#include "activities/Activity.h"

// refreshed UI frame 13: a single 604 px bill for one chapter. It replaces the
// generic two-button confirmation so the reader sees the work, chapter, price
// and the "this chapter only" rule before the payment-password keyboard.
class JjwxcPurchaseConfirmActivity final : public Activity {
 public:
  JjwxcPurchaseConfirmActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string work,
                               std::string chapter, uint32_t points)
      : Activity("JjwxcPurchase", renderer, mappedInput),
        work_(std::move(work)),
        chapter_(std::move(chapter)),
        points_(points) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void confirm();
  void cancel();
  void drawLock(int x, int y, int size);

  std::string work_;
  std::string chapter_;
  uint32_t points_ = 0;
  Rect confirmRect_{};
  Rect cancelRect_{};
};
