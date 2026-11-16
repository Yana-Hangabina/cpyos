#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <I18nKeys.h>

#include "activities/Activity.h"

/*
 * Bluetooth audio page for the Metalio E-INK4 Plus.
 *
 * The board has no Bluetooth stack of its own: a BH1098G SoC owns the radio and
 * is driven with AT commands over UART1. Its mode 1 is the local I2S sink the
 * port boots with (sound feedback); pairing a headset needs mode 2, and that is
 * the only mode in which the vendor page will run AT+INQUIRING / AT+CONNECT
 * (metalio-vendor: bluetooth_screen.cc, MIT - ported literally, command set
 * documented in plus_audio.h).
 *
 * Page lifecycle: enter -> mode 2 (I2S and PA stopped first, so a cue can never
 * straddle the switch) -> inquiry; leave -> mode 1 restored. The mode switch and
 * the AT commands run on a worker task, never on the UI task: one switch costs
 * the vendor 700 ms plus the "SET MODE" ack, and the UI must stay responsive.
 *
 * Long-press on a device row disconnects: AT+BTSCO=0 (music profile) followed by
 * the mode 1 restore. The vendor's music button also sends AT+PP=1 (resume
 * playback) after AT+BTSCO=0; that extra step is deliberately not sent when
 * disconnecting.
 *
 * No cloud and no persistence: the list is per visit and the module keeps its own
 * pairing state.
 */
class PlusBtAudioActivity final : public Activity {
 public:
  explicit PlusBtAudioActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PlusBtAudio", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void prepareForSleep() override;

  // Serial hooks (main.cpp drives the page without touching the panel).
  void debugConfirm() override;
  void debugCancel() override;

 public:
  /* Job kinds are also the worker hand-off protocol (plus_bt_audio_activity.cpp
   * runs them from a task), so they are visible to that translation unit. */
  enum class Job : uint8_t { None, Prepare, Rescan, Connect, Disconnect };

 private:
  enum class Phase : uint8_t { Preparing, Scanning, Idle, Connecting, Connected, Failed };

  struct Device {
    char address[13]; /* 12 hex digits, as the module reports them */
    char name[48];    /* module name, possibly empty */
  };

  static constexpr int kMaxDevices = 16;
  /* Row 0 is the rescan action; devices follow, so row = index + 1. */
  static constexpr int kRescanRow = 0;

  void startJob(Job job, const char* address);
  void finishJob();
  bool workerBusy() const;
  void drainModule();
  void handleLine(const char* line);
  void addDevice(const char* address, const char* name);
  void moveSelection(int delta);
  void activateRow(int row);
  void longPressRow(int row);
  void rescan();
  void disconnect();
  void restoreMode1();
  void setFailed(StrId reason);
  void notifyUpdate();

  int rowCount() const;
  int rowHeight() const;
  int listTop() const;
  int listHeight() const;
  void clampSelection();
  const char* statusText(char* buf, size_t cap) const;
  std::string rowTitleOf(int row) const;
  std::string rowSubtitleOf(int row) const;

  Device devices_[kMaxDevices] = {};
  int deviceCount_ = 0;
  int selected_ = 0;
  int pendingConnectIndex_ = -1;
  int connectedIndex_ = -1; /* index into devices_, -1 when not connected */
  Phase phase_ = Phase::Preparing;
  StrId failReason_ = StrId::STR_BT_CONNECTION_FAILED;
  bool needsPrepare_ = false; /* set when the page is resumed after a sleep */
  bool exiting_ = false;      /* onExit(): suppress updates for a dying activity */
  unsigned long lastDrainMs_ = 0;

  TaskHandle_t worker_ = nullptr;
  Job job_ = Job::None;
};
