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
  bool isJpeg() const;
  bool preparePreview();
  void doSetSleepCover(const char* sourcePath, bool transparent);
  // JPEG sleep covers are decoded straight into a screen-covering BMP, so the
  // FIT/CROP preference is applied at generation time (BMP and PNG sources are
  // placed/cropped by SleepActivity at draw time instead).
  void doSetJpegSleepCover();
  // Shared tail of both cover paths: moves /sleep.bmp.part into place and
  // persists the custom-sleep mode. `prepared` is the decode/copy result.
  void commitSleepImage(bool prepared, bool transparent);
  void showSleepCoverOptions();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool imageReady = false;
  OptionPopup sleepCoverPopup;
};
