#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "components/Rect.h"

// GC2145 still camera page (Metalio E-INK4 Plus).
//
// Opening the page probes the sensor once (plus_camera_probe()); Confirm takes
// one 640x480 photo, writes it to /photos/IMG_<epoch>.jpg and shows it on the
// 16-level canvas. Back returns to the tool drawer.
//
// Every camera call de-initialises and re-initialises the EPD's LCD_CAM, so
// this activity takes the render lock around those calls and forces a full
// refresh for the first frame afterwards (see plus_camera.h).
class PlusCameraActivity final : public Activity {
 public:
  PlusCameraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PlusCamera", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Probing, Ready, Unavailable, Failed, Capturing, Shot };

  bool canCapture() const { return state_ == State::Ready || state_ == State::Failed; }
  void probeSensor();
  void capturePhoto();
  bool saveJpeg(const uint8_t* data, size_t len);
  void drawStatusPage();
  // Returns false when the 16-level pass could not be completed; the caller
  // then draws the status page instead.
  bool drawPhotoPage();
  Rect photoBounds() const;

  State state_ = State::Probing;
  // Saved path on success, failure reason otherwise (ASCII diagnostics only).
  std::string detail_;
  uint16_t pid_ = 0;
  size_t savedBytes_ = 0;
  // Set after any camera call: the panel's LCD_CAM was just rebuilt, so the
  // next frame has to be a full sweep (see plus_camera.h).
  bool forceFullNext_ = true;
};
