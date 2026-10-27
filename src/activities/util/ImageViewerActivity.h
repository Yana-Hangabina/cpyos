#pragma once

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ImageViewerActivity final : public Activity {
 public:
  ImageViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  void loadSiblingImages();
  // CPY: the design's viewer chrome -- round back button, corner page
  // number, and always-on pager buttons for the touch board (swipes are
  // expensive on e-ink). Drawn in every pass of the grayscale render, so
  // it lands on the BW base and both planes like the button hints do.
  void drawCpyChrome(bool hasPrevious, bool hasNext) const;
  bool isPng() const;
  bool preparePreview();
  void doSetSleepCover(const char* sourcePath, bool transparent);
  void showSleepCoverOptions();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool imageReady = false;
  OptionPopup sleepCoverPopup;
};
