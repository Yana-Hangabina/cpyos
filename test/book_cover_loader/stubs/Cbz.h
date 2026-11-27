#pragma once

#include <string>

#include "CoverStub.h"

class Cbz {
 public:
  Cbz(std::string path, const std::string&) : path(std::move(path)) {}

  bool load() const { return path.find("load-fail") == std::string::npos; }
  void setupCacheDir() const { Storage.mkdir("/.crosspoint/cbz"); }
  std::string getThumbBmpPath(int height) const { return "/.crosspoint/cbz/thumb_" + std::to_string(height) + ".bmp"; }
  std::string getCoverBmpPath() const { return "/.crosspoint/cbz/cover.bmp"; }
  bool generateThumbBmp(int height) const {
    ++cover_stub::cbzThumbnailGenerations;
    return cover_stub::writeBmp(getThumbBmpPath(height));
  }
  bool generateCoverBmp() const {
    ++cover_stub::cbzCoverGenerations;
    return cover_stub::writeBmp(getCoverBmpPath());
  }
  const std::string& getTitle() const {
    static const std::string title = "CBZ title";
    return title;
  }

 private:
  std::string path;
};
