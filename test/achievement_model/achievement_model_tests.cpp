#include <gtest/gtest.h>

#include "src/achievements/AchievementModel.h"

TEST(AchievementModel, UnlocksReadingThresholdsWithoutUnlockingFutureMilestones) {
  AchievementSnapshot snapshot;
  snapshot.pages = 100;
  snapshot.readingSeconds = 3600;
  snapshot.sessions = 10;
  snapshot.completedBooks = 1;
  snapshot.longestStreak = 7;
  snapshot.nightSeconds = 3599;

  const uint32_t mask = evaluateAchievementMask(snapshot);
  EXPECT_NE(mask & achievementBit(AchievementId::FirstPage), 0u);
  EXPECT_NE(mask & achievementBit(AchievementId::PageTurner), 0u);
  EXPECT_NE(mask & achievementBit(AchievementId::QuietHour), 0u);
  EXPECT_NE(mask & achievementBit(AchievementId::RegularReader), 0u);
  EXPECT_NE(mask & achievementBit(AchievementId::FirstFinish), 0u);
  EXPECT_NE(mask & achievementBit(AchievementId::SevenDayStreak), 0u);
  EXPECT_EQ(mask & achievementBit(AchievementId::ThousandPages), 0u);
  EXPECT_EQ(mask & achievementBit(AchievementId::DeepReader), 0u);
  EXPECT_EQ(mask & achievementBit(AchievementId::NightOwl), 0u);
}

TEST(AchievementModel, CountsUniqueBookFormats) {
  AchievementSnapshot snapshot;
  snapshot.interactions.formatsOpened = (1u << static_cast<uint8_t>(AchievementBookFormat::Epub)) |
                                        (1u << static_cast<uint8_t>(AchievementBookFormat::Pdf));
  EXPECT_EQ(achievementMetricValue(AchievementMetric::FormatsOpened, snapshot), 2u);
  EXPECT_EQ(evaluateAchievementMask(snapshot) & achievementBit(AchievementId::FormatExplorer), 0u);

  snapshot.interactions.formatsOpened |= 1u << static_cast<uint8_t>(AchievementBookFormat::Text);
  EXPECT_NE(evaluateAchievementMask(snapshot) & achievementBit(AchievementId::FormatExplorer), 0u);
}

TEST(AchievementModel, InteractionThresholdsRequireSuccessfulCounts) {
  AchievementSnapshot snapshot;
  snapshot.interactions.dictionaryLookups = 9;
  snapshot.interactions.bookmarksAdded = 10;
  snapshot.interactions.booksImported = 1;

  const uint32_t mask = evaluateAchievementMask(snapshot);
  EXPECT_EQ(mask & achievementBit(AchievementId::WordHunter), 0u);
  EXPECT_NE(mask & achievementBit(AchievementId::BookmarkKeeper), 0u);
  EXPECT_NE(mask & achievementBit(AchievementId::BookCourier), 0u);
}

TEST(AchievementModel, EveryDefinitionHasARepresentableBit) {
  static_assert(achievementCount() <= 32);
  uint32_t mask = 0;
  for (size_t i = 0; i < achievementCount(); ++i) mask |= achievementBit(static_cast<AchievementId>(i));
  EXPECT_EQ(achievementPopcount(mask), achievementCount());
}
