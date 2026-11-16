#include <gtest/gtest.h>

#include "TestSupport.h"
#include "UserGuide.generated.h"
#include "UserGuide.h"

namespace {
constexpr const char* MARKER = "/.crosspoint/cpyos-user-guide.checked";
constexpr const char* TEMP = "/.crosspoint/user-guide.epub.tmp";
constexpr const char* MARKER_TEMP = "/.crosspoint/cpyos-user-guide.checked.tmp";
using namespace bundled_user_guide;

class UserGuideTest : public ::testing::Test {
 protected:
  void SetUp() override {
    guide_test::files.clear();
    guide_test::failWrite.clear();
    guide_test::failRead.clear();
    guide_test::failRename.clear();
    guide_test::corruptOnFlush.clear();
    guide_test::writeLimit = SIZE_MAX;
    guide_test::bytesWritten = 0;
    guide_test::failRecents = false;
    guide_test::libraryDirty = false;
    RECENT_BOOKS.books.clear();
    RECENT_BOOKS.persisted.clear();
    UserGuide::prepare(true);
  }

  void reboot(bool recentsLoaded = true) {
    RECENT_BOOKS.books = RECENT_BOOKS.persisted;
    UserGuide::prepare(recentsLoaded);
  }

  void expectInstalled(const Asset& asset) {
    ASSERT_TRUE(Storage.exists(asset.path));
    EXPECT_EQ(guide_test::files[asset.path], std::vector<uint8_t>(asset.data, asset.data + asset.size));
    ASSERT_EQ(RECENT_BOOKS.persisted.size(), 1u);
    const auto& book = RECENT_BOOKS.persisted.front();
    EXPECT_EQ(book.path, asset.path);
    EXPECT_EQ(book.title, asset.title);
    EXPECT_EQ(book.author, asset.author);
    EXPECT_FALSE(book.coverBmpPath.empty());
    EXPECT_TRUE(Storage.exists(MARKER));
    EXPECT_TRUE(guide_test::libraryDirty);
  }
};

TEST_F(UserGuideTest, FreshCardWaitsForHomeThenInstallsOnlySelectedLanguage) {
  EXPECT_TRUE(guide_test::files.empty());
  UserGuide::installIfPending(true);
  expectInstalled(Chinese);
  EXPECT_FALSE(Storage.exists(English.path));
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(English.path));
}

TEST_F(UserGuideTest, OtherLanguagesUseEnglishAndUnopenedBooksDoNotPreventSeeding) {
  guide_test::files["/unopened.epub"] = {'b'};
  reboot();
  UserGuide::installIfPending(false);
  expectInstalled(English);
  EXPECT_EQ(guide_test::files["/unopened.epub"], std::vector<uint8_t>{'b'});
}

TEST_F(UserGuideTest, ExistingRecentsGetTheGuideOnceAtTheFront) {
  guide_test::files["/book.epub"] = {'b'};
  RECENT_BOOKS.addBook("/book.epub", "Book", "Author", "cover");
  reboot();
  EXPECT_FALSE(Storage.exists(MARKER));
  UserGuide::installIfPending(true);
  ASSERT_TRUE(Storage.exists(Chinese.path));
  EXPECT_EQ(guide_test::files[Chinese.path], std::vector<uint8_t>(Chinese.data, Chinese.data + Chinese.size));
  EXPECT_TRUE(Storage.exists(MARKER));
  EXPECT_TRUE(guide_test::libraryDirty);
  ASSERT_EQ(RECENT_BOOKS.persisted.size(), 2u);
  EXPECT_EQ(RECENT_BOOKS.persisted[0].path, Chinese.path);
  EXPECT_EQ(RECENT_BOOKS.persisted[0].author, Chinese.author);
  EXPECT_EQ(RECENT_BOOKS.persisted[1].path, "/book.epub");

  // After the marker exists, clearing recents must not pull the guide back.
  guide_test::libraryDirty = false;
  RECENT_BOOKS.persisted.erase(RECENT_BOOKS.persisted.begin());
  reboot();
  UserGuide::installIfPending(false);
  ASSERT_EQ(RECENT_BOOKS.persisted.size(), 1u);
  EXPECT_EQ(RECENT_BOOKS.persisted[0].path, "/book.epub");
  EXPECT_FALSE(guide_test::libraryDirty);
}

TEST_F(UserGuideTest, MissingRecentDoesNotPreventSeeding) {
  RECENT_BOOKS.persisted.push_back({"/deleted.epub", "Deleted", "", ""});
  reboot();
  UserGuide::installIfPending(true);
  expectInstalled(Chinese);
}

TEST_F(UserGuideTest, CorruptOrUnreadableRecentsAreNotOverwritten) {
  guide_test::files[RecentBooksStore::getFilePath()] = {'{'};
  reboot(false);
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(English.path));
  EXPECT_FALSE(Storage.exists(MARKER));
  EXPECT_FALSE(guide_test::libraryDirty);
  EXPECT_EQ(guide_test::files[RecentBooksStore::getFilePath()], std::vector<uint8_t>{'{'});
}

TEST_F(UserGuideTest, RemovingDeletingOrMovingGuideNeverRestoresIt) {
  UserGuide::installIfPending(true);
  RECENT_BOOKS.books.clear();
  RECENT_BOOKS.persisted.clear();
  Storage.rename(Chinese.path, "/moved.epub");
  reboot();
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(English.path));
  EXPECT_FALSE(Storage.exists(Chinese.path));
  Storage.remove("/moved.epub");
  reboot();
  UserGuide::installIfPending(true);
  EXPECT_FALSE(Storage.exists(Chinese.path));
}

TEST_F(UserGuideTest, ShortWriteLeavesNoRecentAndRetriesOnlyAfterReboot) {
  guide_test::failWrite = TEMP;
  guide_test::writeLimit = 300;
  UserGuide::installIfPending(false);
  EXPECT_EQ(guide_test::files[TEMP].size(), 300u);
  EXPECT_FALSE(Storage.exists(English.path));
  EXPECT_FALSE(Storage.exists(MARKER));
  EXPECT_TRUE(RECENT_BOOKS.books.empty());
  guide_test::failWrite.clear();
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(English.path));
  reboot();
  UserGuide::installIfPending(false);
  expectInstalled(English);
}

TEST_F(UserGuideTest, FailedVerificationAndRenameNeverPublishPartialBook) {
  guide_test::corruptOnFlush = TEMP;
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(English.path));
  EXPECT_TRUE(RECENT_BOOKS.books.empty());
  guide_test::corruptOnFlush.clear();
  guide_test::failRename = English.path;
  reboot();
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(English.path));
  EXPECT_FALSE(Storage.exists(MARKER));
  guide_test::failRename.clear();
  reboot();
  UserGuide::installIfPending(false);
  expectInstalled(English);
}

TEST_F(UserGuideTest, ExistingDifferentFileIsPreservedAndNotAdded) {
  guide_test::files[English.path] = {'u', 's', 'e', 'r'};
  UserGuide::installIfPending(false);
  EXPECT_EQ(guide_test::files[English.path], (std::vector<uint8_t>{'u', 's', 'e', 'r'}));
  EXPECT_TRUE(RECENT_BOOKS.books.empty());
  EXPECT_TRUE(Storage.exists(MARKER));
  EXPECT_FALSE(guide_test::libraryDirty);
}

TEST_F(UserGuideTest, ExistingUnreadableFileDefersWithoutMarkingComplete) {
  guide_test::files[English.path] = std::vector<uint8_t>(English.data, English.data + English.size);
  guide_test::failRead = English.path;
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(MARKER));
  EXPECT_TRUE(RECENT_BOOKS.books.empty());
  guide_test::failRead.clear();
  reboot();
  UserGuide::installIfPending(false);
  expectInstalled(English);
}

TEST_F(UserGuideTest, InterruptedRecentSaveReusesVerifiedBookOnReboot) {
  guide_test::failRecents = true;
  UserGuide::installIfPending(false);
  EXPECT_TRUE(Storage.exists(English.path));
  EXPECT_FALSE(Storage.exists(MARKER));
  EXPECT_TRUE(RECENT_BOOKS.persisted.empty());
  guide_test::failRecents = false;
  reboot();
  UserGuide::installIfPending(false);
  expectInstalled(English);
}

TEST_F(UserGuideTest, InterruptedMarkerNeverDuplicatesOrChangesLanguage) {
  guide_test::failWrite = MARKER_TEMP;
  guide_test::writeLimit = 1;
  UserGuide::installIfPending(true);
  EXPECT_FALSE(Storage.exists(MARKER));
  ASSERT_EQ(RECENT_BOOKS.persisted.size(), 1u);
  guide_test::failWrite.clear();
  reboot();
  UserGuide::installIfPending(false);
  expectInstalled(Chinese);
  EXPECT_FALSE(Storage.exists(English.path));
}

TEST_F(UserGuideTest, MarkerRenameFailureIsCompletedOnNextBoot) {
  guide_test::failRename = MARKER;
  UserGuide::installIfPending(false);
  EXPECT_FALSE(Storage.exists(MARKER));
  ASSERT_EQ(RECENT_BOOKS.persisted.size(), 1u);
  guide_test::failRename.clear();
  reboot();
  UserGuide::installIfPending(false);
  expectInstalled(English);
}
}  // namespace
