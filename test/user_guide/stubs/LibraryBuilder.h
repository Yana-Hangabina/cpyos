#pragma once

#include "TestSupport.h"

namespace library {
inline bool markLibraryIndexDirty() {
  guide_test::libraryDirty = true;
  return true;
}
}  // namespace library
