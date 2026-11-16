#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>

#include "InputModality.h"
#include "touch_pair.h"

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#ifndef EPD_SCLK
#define EPD_SCLK 8 // SPI Clock
#endif
#ifndef EPD_MOSI
#define EPD_MOSI 10 // SPI MOSI (Master Out Slave In)
#endif
#ifndef EPD_CS
#define EPD_CS 21 // Chip Select
#endif
#ifndef EPD_DC
#define EPD_DC 4 // Data/Command
#endif
#ifndef EPD_RST
#define EPD_RST 5 // Reset
#endif
#ifndef EPD_BUSY
#define EPD_BUSY 6 // Busy
#endif

#define SPI_MISO                                                               \
  7 // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0 // Battery voltage

#define UART0_RXD 20 // Used for USB connection detection

class HalGPIO {
public:
  enum class DeviceType : uint8_t { X4, X3 };

private:
  DeviceType _deviceType = DeviceType::X4;

  // Edge/level state produced by the FT6336U + cover-key classifier in
  // HalGPIO.cpp. Cleared by beginFrame() like the SDK InputManager latches.
  uint8_t pressedMask_ = 0;
  uint8_t releasedMask_ = 0;
  int heldButton_ = -1;
  // Debounce for the TCA9555 key port: a touch scan on the shared I2C1
  // bus occasionally returned a one-frame glitch, which surfaced as a
  // spurious power press+release and opened Standby on a random tap.
  // Only a sample that stays stable for two polls may change the keys.
  uint8_t port1Raw_ = 0xFF;
  uint8_t port1Stable_ = 0xFF;
  uint8_t port1StableCount_ = 0;
  // Side volume keys: up/down mapping vs their own volume events.
  bool sideKeysNavigate_ = true;
  bool volUpNow_ = false;
  bool volDownNow_ = false;
  bool volUpPrev_ = false;
  bool volDownPrev_ = false;
  bool volUpPressed_ = false;
  bool volUpReleased_ = false;
  bool volDownPressed_ = false;
  bool volDownReleased_ = false;
  unsigned long heldSince_ = 0;
  unsigned long lastHeldMs_ = 0;
  bool screenDown_ = false;
  bool suppressed_ = false;
  bool tapSuppressForContact_ = false;
  bool moved_ = false;
  bool longFired_ = false;
  bool touchDownEvent_ = false;
  bool touchReleasedEvent_ = false;
  bool touchLongPressEvent_ = false;
  bool touchTapEvent_ = false;
  bool swipeEvent_ = false;
  bool pinchGesture_ = false;  // suppress the tap/swipe a pinch would otherwise leave behind
  bool usbStateChanged_ = false;
  bool usbConnected_ = false;
  unsigned long downMs_ = 0;
  int downLx_ = 0;
  int downLy_ = 0;
  int lastLx_ = 0;
  int lastLy_ = 0;
  int swipeLx0_ = 0;
  int swipeLy0_ = 0;
  int swipeLx1_ = 0;
  int swipeLy1_ = 0;
  int pendingTapLx_ = -1;
  int pendingTapLy_ = -1;
  bool injectedDown_ = false;
  bool injectedLongFired_ = false;
  bool pendingInjectDown_ = false;
  bool pendingInjectUp_ = false;
  int pendingInjectLx_ = 0;
  int pendingInjectLy_ = 0;
  int pendingKey_ = -1;
  int pendingReleaseKey_ = -1;
  int pendingSwipe_[4] = {-1, -1, -1, -1};
  // Two-finger pinch state (see touch_pair.h). The event is consumed by
  // wasMultiTouchPinch(), which is const in the shared HAL interface.
  uint32_t pairSeq_ = 0;
  plus_touch_pair::State pinchState_{};
  mutable bool pinchPending_ = false;
  mutable float pinchScale_ = 1.0f;
  mutable float pinchCx_ = 0.0f;
  mutable float pinchCy_ = 0.0f;

public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();
  void beginInput() {}

  // Clears the per-frame press/release edge latches. Must be called exactly
  // once per frame (before the firmware's loop()), NOT on every update().
  void beginFrame();
  // Debug/test injection, applied at the next beginFrame(): a synthetic tap in
  // logical portrait coordinates, or a button press that is released on the
  // following frame. Lets the serial console drive the UI for automated
  // screenshot/log walks.
  void debugInjectTap(int lx, int ly);
  void debugInjectKey(uint8_t buttonIndex);
  void debugInjectSwipe(int x0, int y0, int x1, int y1);
  // Console injection of a *held* contact: the touch stays down across frames
  // (so the long-press timer can fire) until debugInjectTouchUp() is sent.
  void debugInjectTouchDown(int lx, int ly);
  void debugInjectTouchUp();
  // One-shot tap edges are cleared by the activity switch in the firmware; the
  // port clears them per frame in beginFrame(), so this is a no-op.
  void clearTouchTapEvent() {}

  // Button input methods
  void update();
  // Haptics: TCA9555 P0.3 drives the motor. Called after the per-frame input
  // latches are populated; a new press edge starts one non-blocking pulse.
  void updateHapticFeedback(uint8_t level);
  void stopHapticFeedback();
  // Explicit one-shot pulse (boot chime), independent of key edges.
  void pulseHaptic(uint16_t durationMs);
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  // Side volume keys (TCA9555 P1.0 / P1.1). The vendor UI uses them as
  // up/down only inside the reader and as volume +/- everywhere else, so the
  // shell enables navigation per frame. While navigation is off they surface
  // as their own edges/holds instead of BTN_UP/BTN_DOWN.
  void setSideKeysNavigate(bool navigate);
  bool wasVolumeUpPressed() const { return volUpPressed_; }
  bool wasVolumeUpReleased() const { return volUpReleased_; }
  bool wasVolumeDownPressed() const { return volDownPressed_; }
  bool wasVolumeDownReleased() const { return volDownReleased_; }
  bool isVolumeUpHeld() const { return volUpNow_; }
  bool isVolumeDownHeld() const { return volDownNow_; }
  // Debounced TCA9555 port-1 byte (P1.7 = power, P1.0/1 = volume). Bench/diag.
  uint8_t debugPort1Stable() const { return port1Stable_; }
  // Bit i set when physical button i produced a new press edge this frame.
  // Shared code (SoundFeedback) uses it exactly as on Read Pico.
  uint8_t physicalPressedMask() const {
    uint8_t mask = 0;
    for (uint8_t i = 0; i < 7; ++i) {
      if (wasPressed(i)) mask |= static_cast<uint8_t>(1u << i);
    }
    return mask;
  }
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  bool isDebouncePending() const { return false; }
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool rawInputActive();
  // Input modality is always Touch on this board; kept for interface parity so
  // the theme can ask without a special case.
  InputModality lastInputModality() const { return InputModality::Touch; }
  bool wasInputModalityChanged() const { return false; }
  bool hasTouch() const;
  bool hasHomeKey() const;
  bool wasHomeKeyPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float &nx, float &ny) const;
  bool wasTouchDown(float &nx, float &ny) const;
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float &nx, float &ny, unsigned long &heldMs) const;
  bool isTouchHeldAt(float &nx, float &ny) const;
  bool wasTouchLongPress(float &nx, float &ny) const;
  void suppressTouchContact();
  // Kill the tap/swipe/long-press this contact would otherwise emit, but keep
  // the held position visible. Used by drag controls drawn over a page (the
  // volume HUD): the drag must not also activate whatever is underneath.
  void suppressTapForCurrentContact() { tapSuppressForContact_ = true; }
  unsigned long lastTouchHeldMs() const;
  bool wasMultiTouchPinch(float &scale, float &nxCenter, float &nyCenter) const;
  bool wasSwipe(float &nxStart, float &nyStart, float &nxEnd,
                float &nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);
  bool restoreTouchAfterDisplayReset();
  bool consumeSimulatorSleepRequest();

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // Verify power button was held long enough after wakeup.
  // The host wake path is synthetic, so verification always succeeds.
  bool verifyPowerButtonWakeup();
  bool verifyPowerButtonWakeup(uint16_t requiredDurationMs,
                               bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;
  void pollUsbState() {}

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio; // Singleton
