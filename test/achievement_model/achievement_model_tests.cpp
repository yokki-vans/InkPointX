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

  const AchievementBits bits = evaluateAchievementBits(snapshot);
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::FirstPage));
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::PageTurner));
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::QuietHour));
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::RegularReader));
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::FirstFinish));
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::SevenDayStreak));
  EXPECT_FALSE(achievementBitIsSet(bits, AchievementId::ThousandPages));
  EXPECT_FALSE(achievementBitIsSet(bits, AchievementId::DeepReader));
  EXPECT_FALSE(achievementBitIsSet(bits, AchievementId::NightOwl));
}

TEST(AchievementModel, CountsUniqueBookFormats) {
  AchievementSnapshot snapshot;
  snapshot.interactions.formatsOpened = (1u << static_cast<uint8_t>(AchievementBookFormat::Epub)) |
                                        (1u << static_cast<uint8_t>(AchievementBookFormat::Pdf));
  EXPECT_EQ(achievementMetricValue(AchievementMetric::FormatsOpened, snapshot), 2u);
  EXPECT_FALSE(achievementBitIsSet(evaluateAchievementBits(snapshot), AchievementId::FormatExplorer));

  snapshot.interactions.formatsOpened |= 1u << static_cast<uint8_t>(AchievementBookFormat::Text);
  EXPECT_TRUE(achievementBitIsSet(evaluateAchievementBits(snapshot), AchievementId::FormatExplorer));
}

TEST(AchievementModel, InteractionThresholdsRequireSuccessfulCounts) {
  AchievementSnapshot snapshot;
  snapshot.interactions.dictionaryLookups = 9;
  snapshot.interactions.bookmarksAdded = 10;
  snapshot.interactions.booksImported = 1;

  const AchievementBits bits = evaluateAchievementBits(snapshot);
  EXPECT_FALSE(achievementBitIsSet(bits, AchievementId::WordHunter));
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::BookmarkKeeper));
  EXPECT_TRUE(achievementBitIsSet(bits, AchievementId::BookCourier));
}

TEST(AchievementModel, CatalogHasAtLeastOneHundredRepresentableAchievements) {
  static_assert(achievementCount() >= 100);
  AchievementBits bits{};
  for (size_t i = 0; i < achievementCount(); ++i) setAchievementBit(bits, static_cast<AchievementId>(i));
  EXPECT_EQ(achievementPopcount(bits), achievementCount());
  for (const auto& definition : ACHIEVEMENT_DEFINITIONS) EXPECT_GT(definition.target, 0u);
}

TEST(AchievementModel, DailyGoalTrackIncludesOneYearMilestone) {
  AchievementSnapshot snapshot;
  snapshot.dailyGoalsCompleted = 365;
  const AchievementBits bits = evaluateAchievementBits(snapshot);
  EXPECT_EQ(achievementMetricValue(AchievementMetric::DailyGoalsCompleted, snapshot), 365u);
  EXPECT_TRUE(achievementBitIsSet(bits, static_cast<AchievementId>(achievementCount() - 1)));
}

TEST(AchievementModel, StampsOnlyNewUnlocksAndPreservesFirstEarnedDate) {
  AchievementUnlockDays days = makeUnknownAchievementUnlockDays();
  AchievementBits newlyUnlocked{};
  setAchievementBit(newlyUnlocked, AchievementId::FirstPage);
  setAchievementBit(newlyUnlocked, AchievementId::QuietHour);

  stampAchievementUnlockDays(days, newlyUnlocked, 9000);
  EXPECT_EQ(days[static_cast<size_t>(AchievementId::FirstPage)], 9000);
  EXPECT_EQ(days[static_cast<size_t>(AchievementId::QuietHour)], 9000);
  EXPECT_EQ(days[static_cast<size_t>(AchievementId::PageTurner)], ACHIEVEMENT_DAY_UNKNOWN);

  stampAchievementUnlockDays(days, newlyUnlocked, 9001);
  EXPECT_EQ(days[static_cast<size_t>(AchievementId::FirstPage)], 9000);
}
