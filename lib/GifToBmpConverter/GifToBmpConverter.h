#pragma once

#include <HalStorage.h>

class Print;

class GifToBmpConverter {
 public:
  // Matches JpegToBmpConverter::Output: covers use Gray2, thumbnails Gray8 (the
  // library/dashboard canvas is 16-level), and Mono1 is kept for callers that
  // really want one bit.
  enum class Output { Mono1, Gray2, Gray8 };

 private:
  static bool gifFileToBmpStreamInternal(HalFile& gifFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         Output output, bool crop = true, bool originalThresholds = false);

 public:
  static bool gifFileToBmpStream(HalFile& gifFile, Print& bmpOut, bool crop = true,
                                 Output output = Output::Gray2, bool originalThresholds = false);
  static bool gifFileToBmpStream(HalFile& gifFile, Print& bmpOut, bool crop, bool originalThresholds);
  static bool gifFileToBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                         bool crop = true, Output output = Output::Gray2);
  static bool gifFileTo1BitBmpStreamWithSize(HalFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
