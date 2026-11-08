#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "src/util/FirmwareIdentity.h"

namespace {

using firmwareidentity::csvHasToken;
using firmwareidentity::filtersCompatibility;
using firmwareidentity::listRequirementsMet;
using firmwareidentity::matchesCapabilities;
using firmwareidentity::matchesFamily;
using firmwareidentity::matchesPluginApiMinimum;

}  // namespace

TEST(FirmwareIdentity, AdvertisesOnlyImplementedCapabilities) {
  EXPECT_TRUE(csvHasToken(firmwareidentity::CAPABILITIES, "catalog.lists"));
  EXPECT_TRUE(csvHasToken(firmwareidentity::CAPABILITIES, "catalog.list_index_mapping"));
  EXPECT_TRUE(csvHasToken(firmwareidentity::CAPABILITIES, "catalog.list_requirements"));
  // Documented upstream capabilities CPYOS does not implement stay unadvertised.
  EXPECT_FALSE(csvHasToken(firmwareidentity::CAPABILITIES, "catalog.notices"));
  EXPECT_FALSE(csvHasToken(firmwareidentity::CAPABILITIES, "catalog.item_details"));
  EXPECT_FALSE(csvHasToken(firmwareidentity::CAPABILITIES, "catalog.compatibility"));
  EXPECT_FALSE(csvHasToken(firmwareidentity::CAPABILITIES, ""));
}

TEST(FirmwareIdentity, MatchesCapabilityAllOf) {
  EXPECT_TRUE(matchesCapabilities(""));
  EXPECT_TRUE(matchesCapabilities("catalog.lists"));
  EXPECT_TRUE(matchesCapabilities("catalog.lists,catalog.list_index_mapping"));
  EXPECT_TRUE(matchesCapabilities(" catalog.lists , catalog.list_requirements "));
  EXPECT_FALSE(matchesCapabilities("catalog.lists,catalog.notices"));
  EXPECT_FALSE(matchesCapabilities("catalog.lists,"));  // trailing empty token
  EXPECT_FALSE(matchesCapabilities(","));
  EXPECT_FALSE(matchesCapabilities("unknown"));
}

TEST(FirmwareIdentity, FamilyRestrictionIsAnExactCsvToken) {
  EXPECT_TRUE(matchesFamily(""));
  EXPECT_TRUE(matchesFamily(firmwareidentity::FAMILY));
  EXPECT_TRUE(matchesFamily(std::string("other,") + firmwareidentity::FAMILY));
  EXPECT_FALSE(matchesFamily("cpyos"));  // the family advertised to plugins
  EXPECT_FALSE(matchesFamily("crosspoints"));
}

TEST(FirmwareIdentity, PluginApiMinimumIsADecimalFloor) {
  EXPECT_TRUE(matchesPluginApiMinimum(""));
  EXPECT_TRUE(matchesPluginApiMinimum("0"));
  EXPECT_TRUE(matchesPluginApiMinimum("1"));
  EXPECT_FALSE(matchesPluginApiMinimum("2"));
  EXPECT_FALSE(matchesPluginApiMinimum("1x"));
  EXPECT_FALSE(matchesPluginApiMinimum("x1"));
  EXPECT_FALSE(matchesPluginApiMinimum("-1"));
  EXPECT_FALSE(matchesPluginApiMinimum("999999999999"));
}

TEST(FirmwareIdentity, FiltersCompatibilityCombinesAllThree) {
  EXPECT_TRUE(filtersCompatibility("", "", ""));
  EXPECT_TRUE(filtersCompatibility(firmwareidentity::FAMILY, "1", "catalog.lists"));
  EXPECT_FALSE(filtersCompatibility("other", "1", "catalog.lists"));
  EXPECT_FALSE(filtersCompatibility(firmwareidentity::FAMILY, "2", ""));
  EXPECT_FALSE(filtersCompatibility(firmwareidentity::FAMILY, "1", "catalog.notices"));
}

TEST(FirmwareIdentity, ListRequirementsAreBoundedAndAllOf) {
  const std::string_view two[] = {"catalog.lists", "catalog.list_index_mapping"};
  EXPECT_TRUE(listRequirementsMet(nullptr, 0));
  EXPECT_TRUE(listRequirementsMet(two, 2));

  const std::string_view unknown[] = {"catalog.lists", "catalog.notices"};
  EXPECT_FALSE(listRequirementsMet(unknown, 2));

  const std::string_view emptyName[] = {""};
  EXPECT_FALSE(listRequirementsMet(emptyName, 1));

  // Exactly 32 names pass; 33 are treated as malformed without walking them.
  std::vector<std::string_view> many(firmwareidentity::MAX_LIST_REQUIREMENTS, "catalog.lists");
  EXPECT_TRUE(listRequirementsMet(many.data(), many.size()));
  many.push_back("catalog.lists");
  EXPECT_FALSE(listRequirementsMet(many.data(), many.size()));
}
