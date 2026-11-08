#pragma once

#include <string>
#include <vector>

class BookMetadataCache {
 public:
  // `linear` comes from the OPF itemref attribute (linear="no" = auxiliary).
  void createSpineEntry(const std::string&, bool linear = true) { linearFlags.push_back(linear); }

  std::vector<bool> linearFlags;
};
