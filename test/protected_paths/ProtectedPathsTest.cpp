#include <gtest/gtest.h>

#include "network/ProtectedPaths.h"

using protectedpaths::isPluginPath;
using protectedpaths::isPrivateDirectory;
using protectedpaths::isSensitivePath;
using protectedpaths::isUndeletableWebPath;

TEST(ProtectedPaths, CredentialStoresAreSensitive) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/opds.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/koreader.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json.tmp"));
  EXPECT_TRUE(isSensitivePath("/.CrossPoint/WIFI.json"));
}

TEST(ProtectedPaths, SpellingVariantsCannotDodgeTheMatch) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/./wifi.json"));
  EXPECT_TRUE(isSensitivePath("//.crosspoint//wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/plugins/../wifi.json"));
  EXPECT_TRUE(isSensitivePath(".crosspoint/opds.json"));
}

TEST(ProtectedPaths, NamesSdFatWouldTrimOrAliasStillMatch) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json."));
  EXPECT_TRUE(isSensitivePath("/.crosspoint./wifi.json"));
  EXPECT_TRUE(isSensitivePath("/ .crosspoint/ wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint ./opds.json"));
  EXPECT_TRUE(isSensitivePath("/CROSSP~1/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/WIFI~1.JSO"));
  EXPECT_FALSE(isSensitivePath("/Books/Photos/IMG~1.JPG"));  // too deep to reach a store
  EXPECT_FALSE(isSensitivePath("/Books/a~b.epub"));
}

TEST(ProtectedPaths, EverythingElseStaysReachable) {
  EXPECT_FALSE(isSensitivePath("/.crosspoint/settings.json"));
  EXPECT_FALSE(isSensitivePath("/.crosspoint/library.idx"));
  EXPECT_FALSE(isSensitivePath("/.crosspoint/month-wallpaper.json"));
  EXPECT_FALSE(isSensitivePath("/.fonts/Noto/Noto_14.cpfont"));
  EXPECT_FALSE(isSensitivePath("/Books/wifi.json"));
}

TEST(ProtectedPaths, HiddenFilesAreDeletableButNotTheDeviceDirectory) {
  // The File Manager may delete the dot-named leftovers users need to clear.
  EXPECT_FALSE(isUndeletableWebPath("/.fonts/.stale"));
  EXPECT_FALSE(isUndeletableWebPath("/.hidden-epub"));
  EXPECT_FALSE(isUndeletableWebPath("/Books/.thumb"));
  // Everything the device owns stays put, including the directory itself.
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint"));
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint/"));
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint/settings.json"));
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint/jjwxc/cookies"));
  EXPECT_TRUE(isUndeletableWebPath("/.CrossPoint/JJwxc/paykey"));
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint/./settings.json"));
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint/plugins/../settings.json"));
  EXPECT_TRUE(isUndeletableWebPath("//.crosspoint//library.idx"));
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint./settings.json"));
  EXPECT_FALSE(isUndeletableWebPath("/.crosspoint-backup/cookies"));
  // Credential stores keep their own rule.
  EXPECT_TRUE(isUndeletableWebPath("/.crosspoint/wifi.json"));
  EXPECT_FALSE(isUndeletableWebPath("/Books/wifi.json"));
}

TEST(ProtectedPaths, PrivateDirectoryShapes) {
  EXPECT_TRUE(isPrivateDirectory("/.crosspoint"));
  EXPECT_TRUE(isPrivateDirectory("/.crosspoint/"));
  EXPECT_TRUE(isPrivateDirectory("/.crosspoint/a"));
  EXPECT_FALSE(isPrivateDirectory("/.crosspoint2"));
  EXPECT_FALSE(isPrivateDirectory("/.crosspoint2/a"));
  EXPECT_FALSE(isPrivateDirectory("/"));
  EXPECT_FALSE(isPrivateDirectory("/Books"));
}

TEST(ProtectedPaths, PluginPathRules) {
  EXPECT_FALSE(isPluginPath("/.crosspoint/plugins/../wifi.json"));
  EXPECT_FALSE(isPluginPath("/.crosspoint/./koreader.json"));
  EXPECT_FALSE(isPluginPath("relative/path"));
  EXPECT_FALSE(isPluginPath("/"));
  EXPECT_TRUE(isPluginPath("/.crosspoint/month-wallpaper.json"));
  EXPECT_TRUE(isPluginPath("/Books/new.epub"));
}
