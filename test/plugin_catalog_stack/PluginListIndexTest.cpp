#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "src/util/PluginListIndex.h"

namespace {

// Minimal stand-in for the catalog's BrowseList: the merge helper only moves
// elements and resizes, so a tag string is enough to observe ordering.
struct List {
  std::string title;
};

std::vector<List> makeLists(const std::size_t count, const char* prefix) {
  std::vector<List> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) out.push_back(List{std::string(prefix) + std::to_string(i)});
  return out;
}

}  // namespace

TEST(PluginListIndex, MergesFetchedAfterFixedLists) {
  std::vector<List> lists = makeLists(2, "fixed");
  auto fetched = makeLists(3, "remote");

  ASSERT_TRUE(pluginlistindex::mergeFetchedLists(lists, 2, std::move(fetched)));
  ASSERT_EQ(lists.size(), 5u);
  EXPECT_EQ(lists[0].title, "fixed0");
  EXPECT_EQ(lists[1].title, "fixed1");
  EXPECT_EQ(lists[2].title, "remote0");
  EXPECT_EQ(lists[4].title, "remote2");
}

TEST(PluginListIndex, FixedListsSurviveAnEmptyOrFailedFetch) {
  std::vector<List> lists = makeLists(2, "fixed");
  std::vector<List> fetched;  // every remote entry was filtered (requires/title/url)

  EXPECT_FALSE(pluginlistindex::mergeFetchedLists(lists, 2, std::move(fetched)));
  ASSERT_EQ(lists.size(), 2u);  // untouched, so the picker still has the fixed lists
  EXPECT_EQ(lists[0].title, "fixed0");
  EXPECT_EQ(lists[1].title, "fixed1");

  // A retry after a failed merge still starts from the fixed lists.
  lists = makeLists(2, "fixed");
  auto replacement = makeLists(1, "retry");
  ASSERT_TRUE(pluginlistindex::mergeFetchedLists(lists, 2, std::move(replacement)));
  ASSERT_EQ(lists.size(), 3u);
  EXPECT_EQ(lists[2].title, "retry0");
}

TEST(PluginListIndex, RetryReplacesTheFetchedTailInsteadOfDuplicating) {
  std::vector<List> lists = makeLists(1, "fixed");
  auto first = makeLists(2, "old");
  ASSERT_TRUE(pluginlistindex::mergeFetchedLists(lists, 1, std::move(first)));
  ASSERT_EQ(lists.size(), 3u);

  auto second = makeLists(2, "new");
  ASSERT_TRUE(pluginlistindex::mergeFetchedLists(lists, 1, std::move(second)));
  ASSERT_EQ(lists.size(), 3u);  // old tail dropped first
  EXPECT_EQ(lists[0].title, "fixed0");
  EXPECT_EQ(lists[1].title, "new0");
  EXPECT_EQ(lists[2].title, "new1");
}

TEST(PluginListIndex, MergeIsBoundedAtSixtyFourLists) {
  std::vector<List> lists = makeLists(pluginlistindex::kMaxBrowseLists, "fixed");
  auto ordinary = makeLists(1, "remote");
  EXPECT_FALSE(pluginlistindex::mergeFetchedLists(lists, pluginlistindex::kMaxBrowseLists, std::move(ordinary)));
  ASSERT_EQ(lists.size(), pluginlistindex::kMaxBrowseLists);

  // A 100-entry index over one fixed list keeps the bound, not the index size.
  lists = makeLists(1, "fixed");
  auto large = makeLists(100, "remote");
  ASSERT_TRUE(pluginlistindex::mergeFetchedLists(lists, 1, std::move(large)));
  EXPECT_EQ(lists.size(), pluginlistindex::kMaxBrowseLists);
  EXPECT_EQ(lists.front().title, "fixed0");
  EXPECT_EQ(lists.back().title, "remote62");  // 1 fixed + 63 remote
}

TEST(PluginListIndex, FixedCountLargerThanTheVectorIsHarmless) {
  std::vector<List> lists = makeLists(1, "fixed");
  auto fetched = makeLists(1, "remote");
  ASSERT_TRUE(pluginlistindex::mergeFetchedLists(lists, 5, std::move(fetched)));
  EXPECT_EQ(lists.size(), 2u);
  EXPECT_EQ(lists[0].title, "fixed0");
}
