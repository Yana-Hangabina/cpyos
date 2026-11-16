#include <gtest/gtest.h>

#include <WeReadReadingClock.h>

using weread_time_sync::ReadingClock;

TEST(ReadingClock, BeginsEmptyAndCountsSamples) {
  ReadingClock clock;
  clock.begin(1000);
  EXPECT_TRUE(clock.started());
  EXPECT_EQ(clock.pendingMs(), 0u);
  EXPECT_TRUE(clock.sample(2000));
  EXPECT_EQ(clock.pendingMs(), 1000u);
  EXPECT_TRUE(clock.sample(2500));
  EXPECT_EQ(clock.pendingMs(), 1500u);
}

TEST(ReadingClock, DiscardsLongGapsAndZeroDelta) {
  ReadingClock clock;
  clock.begin(0);
  ASSERT_TRUE(clock.sample(1000));
  EXPECT_FALSE(clock.sample(1000));                       // no time passed
  EXPECT_FALSE(clock.sample(1000 + 60001));               // left the book
  EXPECT_EQ(clock.pendingMs(), 1000u);                    // neither was counted
  EXPECT_TRUE(clock.sample(1000 + 60001 + 500));          // normal sampling resumes
  EXPECT_EQ(clock.pendingMs(), 1500u);
}

TEST(ReadingClock, SkipToMovesCursorWithoutCounting) {
  ReadingClock clock;
  clock.begin(0);
  ASSERT_TRUE(clock.sample(500));
  clock.skipTo(60000);  // menu / first-run hint
  EXPECT_EQ(clock.pendingMs(), 500u);
  EXPECT_TRUE(clock.sample(60500));
  EXPECT_EQ(clock.pendingMs(), 1000u);
}

TEST(ReadingClock, TakeSecondsKeepsSubSecondRemainder) {
  ReadingClock clock;
  clock.begin(0);
  ASSERT_TRUE(clock.sample(1500));
  EXPECT_EQ(clock.takeSeconds(), 1u);
  EXPECT_EQ(clock.pendingMs(), 500u);
  ASSERT_TRUE(clock.sample(2100));
  EXPECT_EQ(clock.takeSeconds(), 1u);
  EXPECT_EQ(clock.pendingMs(), 100u);
  EXPECT_EQ(clock.takeSeconds(), 0u);  // nothing whole left
}

TEST(ReadingClock, TickCadence) {
  ReadingClock clock;
  clock.begin(0);
  EXPECT_FALSE(clock.tickDue(29999));
  EXPECT_TRUE(clock.tickDue(30000));
  clock.markTick(30000);
  EXPECT_FALSE(clock.tickDue(30000));
  EXPECT_FALSE(clock.tickDue(59999));
  EXPECT_TRUE(clock.tickDue(60000));
}

TEST(ReadingClock, StopHaltsSamplingAndTicks) {
  ReadingClock clock;
  clock.begin(0);
  ASSERT_TRUE(clock.sample(1000));
  clock.stop();
  EXPECT_FALSE(clock.started());
  EXPECT_FALSE(clock.sample(2000));
  EXPECT_EQ(clock.pendingMs(), 1000u);
  EXPECT_FALSE(clock.tickDue(30000));
}
