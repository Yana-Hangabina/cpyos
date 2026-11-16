#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <I18nKeys.h>

#include "activities/Activity.h"
#include "components/InlineConfirm.h"

/*
 * Local voice memo page for the Metalio E-INK4 Plus.
 *
 * The board's microphone and speaker both ride the BH1098G I2S link, which only
 * clocks audio in the vendor "mode 1" (plus_audio.h). Capture averages the
 * interleaved stereo pairs that plus_audio_read_mic() returns (mic + reference)
 * into one mono 16-bit stream and writes a plain 44-byte-header WAV, so the
 * files open in any desktop player:
 *
 *   /recordings/memo_<epoch>.wav   16 kHz, mono, PCM s16le
 *
 * Nothing audio-related runs on the UI task. Each job is one short-lived worker
 * with an 8 KiB INTERNAL stack (never PSRAM: the worker writes the SD card, and
 * a PSRAM-stack task may not touch internal flash or park the card). The worker
 * self-deletes with vTaskDelete(nullptr), which is the matching API for
 * xTaskCreatePinnedToCore(); it only publishes atomics, so a worker that outlives
 * the page can never dereference freed memory, and the activity never deletes a
 * live handle.
 *
 * Rows: row 0 action = start a new recording, rows 1..N = newest-first memos.
 * Confirm records/plays, Back leaves the page (stopping playback first), long
 * press deletes a file behind an inline confirm.
 */
class PlusVoiceMemoActivity final : public Activity {
 public:
  PlusVoiceMemoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PlusVoiceMemo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void prepareForSleep() override;
  bool preventAutoSleep() override;

  // Serial hooks (main.cpp drives the page without touching the panel).
  void debugConfirm() override;
  void debugCancel() override;

  /* Job kinds are the worker hand-off protocol (plus_voice_memo_activity.cpp runs
   * them from a task), so they are visible to that translation unit. */
  enum class Job : uint8_t { None, Record, Play };

 private:
  enum class Phase : uint8_t { List, Recording, Playing, Failed };

  struct Memo {
    char name[32]; /* file name inside kDir, extension included */
    uint32_t epoch;
    uint32_t bytes;
    uint32_t seconds;
  };

  static constexpr int kMaxMemos = 40;
  /* Row 0 is the record action; memos follow, so row = index + 1. */
  static constexpr int kActionRow = 0;

  void loadMemos();
  void startRecording();
  void startPlayback(int index);
  void startJob(Job job);
  void finishJob();
  bool workerBusy() const;
  void stopWorker();
  bool reapWorker(uint32_t timeoutMs);

  void moveSelection(int delta);
  void activateRow(int row);
  void longPressRow(int row);

  int rowCount() const;
  int rowHeight() const;
  int listTop() const;
  int listHeight() const;
  std::string rowTitleOf(int row) const;
  std::string rowSubtitleOf(int row) const;

  Phase phase_ = Phase::List;
  /* Shown inside the empty list when the page is in Phase::Failed. */
  StrId failedStr_ = StrId::STR_NOT_AVAILABLE;
  /* One-shot result of the last playback attempt, shown in the sub-header. */
  StrId notice_ = StrId::STR_NONE_OPT;
  Memo memos_[kMaxMemos] = {};
  int memoCount_ = 0;
  int selected_ = kActionRow;
  /* Last whole second pushed to the panel; the counters are otherwise polled. */
  uint32_t lastTickSecond_ = 0;
  bool exiting_ = false; /* onExit(): suppress updates for a dying activity */

  TaskHandle_t worker_ = nullptr;
  Job job_ = Job::None;
  InlineConfirm confirm_;
};
