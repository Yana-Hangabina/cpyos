#pragma once

// Bounded merge of a remote plugin list index into a manifest's fixed lists.
// Header-only so the host tests exercise the exact rules the catalog uses
// (test/plugin_catalog_stack) and the 64-entry limit lives in one place.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace pluginlistindex {

// Hard bound for the browse-list picker. A fetched index is a 16 KiB document
// of short entries; 64 merged lists already run far past one screen, and each
// entry owns several strings. Keep this as the bound on both the reserve and
// the append loop.
inline constexpr std::size_t kMaxBrowseLists = 64;

// Appends `fetched` after the manifest's own leading `fixedCount` lists.
//
// A failed or empty fetch returns false and leaves `lists` untouched: that is
// how the manifest's fixed lists survive an unreachable index. On success, any
// previously fetched tail is dropped first so a retry cannot duplicate entries.
template <typename List>
bool mergeFetchedLists(std::vector<List>& lists, const std::size_t fixedCount, std::vector<List>&& fetched) {
  if (fetched.empty()) return false;
  if (fixedCount < lists.size()) lists.resize(fixedCount);
  const std::size_t room = lists.size() < kMaxBrowseLists ? kMaxBrowseLists - lists.size() : 0;
  const std::size_t take = std::min(room, fetched.size());
  if (take == 0) return false;
  lists.reserve(lists.size() + take);  // bounded by kMaxBrowseLists
  for (std::size_t i = 0; i < take; ++i) lists.push_back(std::move(fetched[i]));
  return true;
}

}  // namespace pluginlistindex
