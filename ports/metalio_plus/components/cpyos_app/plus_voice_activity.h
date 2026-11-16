#pragma once

#include <string>

#include <I18nKeys.h>

#include "activities/Activity.h"
#include "plus_voice_client.h"

/*
 * Tools-drawer page for the push-to-talk AI-voice client (Metalio E-INK4 Plus
 * only). Rows: server URL, token (masked), Connect/Disconnect, Talk.
 *
 * The worker lives in plus_voice_client.cpp; this page only edits
 * /.crosspoint/voice.json (see that header for the format), brings Wi-Fi up,
 * starts/stops the session, and mirrors the atomics into the panel. Nothing
 * audio- or network-related runs on the UI task, and every input path is
 * bounded: holding the Talk row's Confirm or the on-screen capsule streams,
 * releasing it stops, Back stops and leaves the page.
 */
class PlusVoiceActivity final : public Activity {
 public:
  // autoTalk is what the physical AI key's long press requests: open the page
  // and begin listening as soon as the channel is up, instead of asking the
  // user to hold a second control while TLS negotiates.
  // fromKey marks a page the AI key pushed on top of the current one (Home,
  // reader, settings, ...). Back then pops to that page; the tools-drawer entry
  // replaces My instead, so its Back keeps going to the app grid.
  explicit PlusVoiceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             bool autoTalk = false, bool fromKey = false)
      : Activity("PlusVoice", renderer, mappedInput), autoTalk_(autoTalk), fromKey_(fromKey) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool onAiKey(bool longPress) override;
  void prepareForSleep() override;

  // Serial hooks (bench harness drives the page without touching the panel).
  void debugConfirm() override;
  void debugCancel() override;

 private:
  enum Row : int { kRowUrl = 0, kRowToken, kRowConnect, kRowTalk, kRowCount };

  void moveSelection(int delta);
  void activateRow(int row);
  void editUrl();
  void editToken();
  void connectOrDisconnect();
  void beginConnect();
  void endSession();
  void onWifiResult(bool connected);
  void pollSession();
  void setTalk(bool held);

  int rowHeight() const;
  Rect listRect() const;
  Rect talkRect() const;
  Rect textRect() const;
  std::string rowTitle(int row) const;
  std::string rowSubtitle(int row) const;
  std::string rowValue(int row) const;
  const char* statusText() const;

  plus_voice::Config config_;
  int selected_ = kRowUrl;
  StrId notice_ = StrId::STR_NONE_OPT;
  plus_voice::Phase lastPhase_ = plus_voice::Phase::Idle;
  bool lastPtt_ = false;
  bool talkHeld_ = false;      // level state pushed to the worker
  bool confirmTalk_ = false;   // this Confirm press started a talk turn
  bool touchTalk_ = false;     // the current turn was started by the capsule
  bool wifiOwned_ = false;     // we brought the radio up; release it on exit
  bool exiting_ = false;
  bool autoTalk_ = false;          // long-press entry: listen once connected
  bool fromKey_ = false;           // pushed by the AI key: Back pops to the caller
  bool pendingAutoTalk_ = false;   // connect in flight, start the turn on open
  bool autoConnectPending_ = false;  // start the session on the first loop
  char output_[128] = {0};
};
