#include "StandbyImageFace.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "CrossPointSettings.h"

namespace {

struct Placement {
  int x = 0;
  int y = 0;
  float cropX = 0;
  float cropY = 0;
};

Placement placementFor(const int bw, const int bh, const GfxRenderer& renderer) {
  Placement p;
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  if (bw <= sw && bh <= sh) {
    p.x = (sw - bw) / 2;
    p.y = (sh - bh) / 2;
    return p;
  }

  float ratio = static_cast<float>(bw) / static_cast<float>(bh);
  const float screenRatio = static_cast<float>(sw) / static_cast<float>(sh);
  if (ratio > screenRatio) {
    if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::CROP) {
      p.cropX = 1.0f - (screenRatio / ratio);
      ratio = (1.0f - p.cropX) * static_cast<float>(bw) / static_cast<float>(bh);
    }
    p.y = static_cast<int>(std::lround((static_cast<float>(sh) - static_cast<float>(sw) / ratio) / 2));
  } else {
    if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::CROP) {
      p.cropY = 1.0f - (ratio / screenRatio);
      ratio = static_cast<float>(bw) / ((1.0f - p.cropY) * static_cast<float>(bh));
    }
    p.x = static_cast<int>(std::lround((static_cast<float>(sw) - static_cast<float>(sh) * ratio) / 2));
  }
  return p;
}

}  // namespace

StandbyImageFace::StandbyImageFace(const char* path) : path_(path ? path : "") {
  if (path_.empty()) return;
  HalFile file;
  if (!Storage.openFileForRead("STBY", path_.c_str(), file)) return;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return;
  width_ = bitmap.getWidth();
  height_ = bitmap.getHeight();
  valid_ = width_ > 0 && height_ > 0;
}

void StandbyImageFace::onEnter() {}

void StandbyImageFace::onExit() {}

StandbyFace::TickResult StandbyImageFace::tick() { return TickResult::None; }

void StandbyImageFace::render(GfxRenderer& renderer, const Rect& viewport) {
  if (!valid_) return;
  HalFile file;
  if (!Storage.openFileForRead("STBY", path_.c_str(), file)) return;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return;

  const Placement p = placementFor(bitmap.getWidth(), bitmap.getHeight(), renderer);
  (void)viewport;  // the image is full-bleed; the activity clipped the viewport
  renderer.drawBitmap(bitmap, p.x, p.y, renderer.getScreenWidth(), renderer.getScreenHeight(), p.cropX, p.cropY,
                      /*preserveTransparency=*/false);
}

StrId StandbyImageFace::titleId() const { return StrId::STR_CUSTOM_IMAGE; }

uint32_t StandbyImageFace::secondsUntilNextWake() const { return 60u; }
