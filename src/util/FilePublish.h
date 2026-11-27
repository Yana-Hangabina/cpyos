#pragma once

#include <string>

#include "FilePublishRules.h"

namespace file_publish {

// Put `source` in place as `dest`, and report exactly which route worked.
//
// The ladder is: rename when the destination is absent; remove-then-rename when
// it is present; overwrite byte for byte when neither rename worked. One of the
// two copies is always left intact -- the source is only deleted after the
// destination is proven, and the destination is only truncated once the card has
// been shown to have room for the new bytes. A stuck destination (a damaged FAT
// entry behaves like this) is reported as DestinationStuck rather than retried
// into silence.
//
// Callers that download-then-verify-then-publish should use this instead of
// writing their own remove+rename pair: the three strategies this tree had
// before it differed only in which FAT failure they could survive, and each new
// call site picked a different one.
//
// Runs on the caller's task. The copy buffer is 1 KiB on purpose: font and
// engine installs run on 16 KiB PSRAM-stack workers.
Outcome replace(const char* logModule, const std::string& source, const std::string& dest);

}  // namespace file_publish
