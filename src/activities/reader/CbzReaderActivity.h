#pragma once

#include <Cbz.h>

#include <memory>
#include <string>

#include "ReaderActivity.h"

class CbzReaderActivity final : public ReaderActivity {
  std::unique_ptr<Cbz> cbz;
  int currentPage = 0;
  int savedFullRefreshPage = -1;

  void renderPage();
  void saveProgress() const;
  void loadProgress();

  bool loadBook() override;
  std::string getBookTitle() const override { return cbz ? cbz->getTitle() : ""; }
  std::string getBookAuthor() const override { return cbz ? cbz->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return cbz ? cbz->getThumbBmpPath(200) : ""; }
  bool handleFormatInput() override;
  void renderBook() override;
  void applyInitialOrientation() override;

 public:
  explicit CbzReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("CbzReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~CbzReaderActivity() override = default;

  void onExit() override;
  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;
  ScreenshotInfo getScreenshotInfo() const override;
};
