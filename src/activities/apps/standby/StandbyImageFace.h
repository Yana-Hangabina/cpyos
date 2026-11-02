#pragma once

#include <string>

#include "StandbyFace.h"

class GfxRenderer;

// Static user image for standby / light-sleep custom styles. The PNG picker in
// Settings converts the source to a 1-bit BMP at a fixed path; this face only
// streams that BMP (no full-frame buffer). It never ticks: a custom image has
// no clock, so light sleep does not repaint it every minute.
class StandbyImageFace final : public StandbyFace {
 public:
  explicit StandbyImageFace(const char* path);
  bool valid() const { return valid_; }
  void onEnter() override;
  void onExit() override;
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override;
  uint32_t secondsUntilNextWake() const override;

 private:
  std::string path_;
  bool valid_ = false;
  int width_ = 0;
  int height_ = 0;
};
