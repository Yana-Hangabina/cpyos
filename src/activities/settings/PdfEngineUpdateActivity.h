#pragma once

#include "PdfUpdateWatchdog.h"

#include <string>

#include "activities/Activity.h"
#include "components/StatusCards.h"
#include "pdf/PdfEngineUpdate.h"

// Everything here depends on the XIP engine: pdf_engine and pdf_module only
// declare their API in a CROSSPOINT_PDF_XIP build.
#if defined(CROSSPOINT_PDF_XIP)

// The PDF engine as a user-updatable component: instead of re-flashing the
// whole device, this fetches the version-pinned engine image, verifies it and
// writes it to the pdfmod partition.
//
// The split is mandatory, not stylistic: the network + SD download + SHA-256
// phases run in pdf_engine's PSRAM-stack worker, while the partition erase and
// write happen here, on this task's internal stack -- writing internal flash
// with the stack in PSRAM trips esp_task_stack_is_sane_cache_disabled() and
// restart-loops (AGENTS.md section 7).
class PdfEngineUpdateActivity final : public Activity {
 public:
  // `returnOnSuccess` is set when the reader opened this to resolve a
  // dependency: on success it pops immediately so the book continues.  Settings
  // keeps the done card.
  PdfEngineUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                          const bool returnOnSuccess = false)
      : Activity("PdfEngineUpdate", renderer, mappedInput), returnOnSuccess_(returnOnSuccess) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase : uint8_t { Connecting, Running, Installing, Finished, Failed };
  Phase phase_ = Phase::Connecting;
  bool radioOwnedByUs_ = false;
  bool cancelRequested_ = false;
  bool returnOnSuccess_ = false;
  uint32_t startedAtMs_ = 0;
  uint32_t lastTickMs_ = 0;
  // A trickle that never advances must not leave the card spinning forever: the
  // download blocks inside the transport, so only the UI task can bound it.
  size_t lastProgressBytes_ = 0;
  // kNoBaseline until the body stage is entered: measuring "no progress" from 0
  // meant from power-on and cancelled transfers that had just started.
  uint32_t lastProgressMs_ = pdf_update::kNoBaseline;
  pdf_update::StallStage lastStage_ = pdf_update::StallStage::Idle;
  status_card::Layout cardLayout_{};
  std::string detail_;
  const char* title_ = nullptr;
  const char* body_ = nullptr;

  void connectAndStart();
  void noteFailure(pdf_engine::Failure failure, const std::string& why);
  void installNow();
  void showResult(const char* resultTitle, const char* resultBody, bool acknowledged);
};

#endif  // CROSSPOINT_PDF_XIP
