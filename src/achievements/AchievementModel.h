#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The first 16 IDs are intentionally stable: v2.2.16 stored them in a uint32_t
// mask. Keeping their positions lets AchievementSystem migrate that file
// without losing anything the reader has already earned.
enum class AchievementId : uint8_t {
  FirstPage = 0,
  PageTurner,
  ThousandPages,
  QuietHour,
  DeepReader,
  RegularReader,
  FirstFinish,
  SevenDayStreak,
  NightOwl,
  WordHunter,
  BookmarkKeeper,
  FormatExplorer,
  Connected,
  FontCollector,
  BookCourier,
  UpToDate,
  Count = 112,
};

enum class AchievementMetric : uint8_t {
  Pages,
  ReadingSeconds,
  Sessions,
  CompletedBooks,
  LongestStreak,
  NightSeconds,
  DictionaryLookups,
  BookmarksAdded,
  FormatsOpened,
  WifiConnections,
  FontsDownloaded,
  BooksImported,
  OtaUpdates,
  DailyGoalsCompleted,
};

enum class AchievementEvent : uint8_t {
  DictionaryLookup,
  BookmarkAdded,
  WifiConnected,
  FontDownloaded,
  BookImported,
  OtaUpdated,
};

enum class AchievementBookFormat : uint8_t { Epub = 0, Fb2, Pdf, Text, Xtc, Count };

struct AchievementCounters {
  uint32_t dictionaryLookups = 0;
  uint32_t bookmarksAdded = 0;
  uint32_t wifiConnections = 0;
  uint32_t fontsDownloaded = 0;
  uint32_t booksImported = 0;
  uint32_t otaUpdates = 0;
  uint8_t formatsOpened = 0;
};

struct AchievementSnapshot {
  uint32_t pages = 0;
  uint32_t readingSeconds = 0;
  uint32_t sessions = 0;
  uint32_t completedBooks = 0;
  uint32_t longestStreak = 0;
  uint32_t nightSeconds = 0;
  uint32_t dailyGoalsCompleted = 0;
  AchievementCounters interactions;
};

struct AchievementDefinition {
  AchievementMetric metric;
  uint32_t target;
};

constexpr size_t achievementCount() { return static_cast<size_t>(AchievementId::Count); }
constexpr size_t ACHIEVEMENT_BIT_BYTES = (achievementCount() + 7) / 8;
using AchievementBits = std::array<uint8_t, ACHIEVEMENT_BIT_BYTES>;

// Calendar day since 2000-01-01.  The whole supported RTC range (2000-2099)
// fits in 16 bits; 0xffff is reserved for achievements earned before date
// tracking existed or while the device clock was unavailable.
constexpr uint16_t ACHIEVEMENT_DAY_UNKNOWN = 0xffff;
using AchievementUnlockDays = std::array<uint16_t, achievementCount()>;

constexpr AchievementUnlockDays makeUnknownAchievementUnlockDays() {
  AchievementUnlockDays days{};
  for (auto& day : days) day = ACHIEVEMENT_DAY_UNKNOWN;
  return days;
}

template <size_t N, size_t E>
constexpr void appendAchievementTrack(std::array<AchievementDefinition, achievementCount()>& definitions, size_t& index,
                                      const AchievementMetric metric, const std::array<uint32_t, N>& targets,
                                      const std::array<uint32_t, E>& existingTargets) {
  for (const uint32_t target : targets) {
    bool exists = false;
    for (const uint32_t existing : existingTargets) {
      if (target == existing) {
        exists = true;
        break;
      }
    }
    if (!exists && index < definitions.size()) definitions[index++] = {metric, target};
  }
}

constexpr std::array<AchievementDefinition, achievementCount()> makeAchievementDefinitions() {
  std::array<AchievementDefinition, achievementCount()> definitions{};
  size_t index = 0;

  // Original v2.2.16 achievements.
  definitions[index++] = {AchievementMetric::Pages, 1};
  definitions[index++] = {AchievementMetric::Pages, 100};
  definitions[index++] = {AchievementMetric::Pages, 1000};
  definitions[index++] = {AchievementMetric::ReadingSeconds, 3600};
  definitions[index++] = {AchievementMetric::ReadingSeconds, 36000};
  definitions[index++] = {AchievementMetric::Sessions, 10};
  definitions[index++] = {AchievementMetric::CompletedBooks, 1};
  definitions[index++] = {AchievementMetric::LongestStreak, 7};
  definitions[index++] = {AchievementMetric::NightSeconds, 3600};
  definitions[index++] = {AchievementMetric::DictionaryLookups, 10};
  definitions[index++] = {AchievementMetric::BookmarksAdded, 10};
  definitions[index++] = {AchievementMetric::FormatsOpened, 3};
  definitions[index++] = {AchievementMetric::WifiConnections, 1};
  definitions[index++] = {AchievementMetric::FontsDownloaded, 1};
  definitions[index++] = {AchievementMetric::BooksImported, 1};
  definitions[index++] = {AchievementMetric::OtaUpdates, 1};

  appendAchievementTrack(definitions, index, AchievementMetric::Pages,
                         std::array<uint32_t, 12>{1, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000},
                         std::array<uint32_t, 3>{1, 100, 1000});
  appendAchievementTrack(
      definitions, index, AchievementMetric::ReadingSeconds,
      std::array<uint32_t, 12>{600, 1800, 3600, 7200, 18000, 36000, 90000, 180000, 360000, 900000, 1800000, 3600000},
      std::array<uint32_t, 2>{3600, 36000});
  appendAchievementTrack(definitions, index, AchievementMetric::Sessions,
                         std::array<uint32_t, 10>{1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500},
                         std::array<uint32_t, 1>{10});
  appendAchievementTrack(definitions, index, AchievementMetric::CompletedBooks,
                         std::array<uint32_t, 10>{1, 2, 5, 10, 20, 30, 50, 75, 100, 200}, std::array<uint32_t, 1>{1});
  appendAchievementTrack(definitions, index, AchievementMetric::LongestStreak,
                         std::array<uint32_t, 10>{2, 3, 5, 7, 14, 21, 30, 60, 100, 365}, std::array<uint32_t, 1>{7});
  appendAchievementTrack(definitions, index, AchievementMetric::NightSeconds,
                         std::array<uint32_t, 8>{600, 1800, 3600, 7200, 18000, 36000, 90000, 180000},
                         std::array<uint32_t, 1>{3600});
  appendAchievementTrack(definitions, index, AchievementMetric::DictionaryLookups,
                         std::array<uint32_t, 8>{1, 5, 10, 25, 50, 100, 250, 500}, std::array<uint32_t, 1>{10});
  appendAchievementTrack(definitions, index, AchievementMetric::BookmarksAdded,
                         std::array<uint32_t, 8>{1, 5, 10, 25, 50, 100, 250, 500}, std::array<uint32_t, 1>{10});
  appendAchievementTrack(definitions, index, AchievementMetric::FormatsOpened, std::array<uint32_t, 5>{1, 2, 3, 4, 5},
                         std::array<uint32_t, 1>{3});
  appendAchievementTrack(definitions, index, AchievementMetric::WifiConnections,
                         std::array<uint32_t, 5>{1, 5, 10, 25, 50}, std::array<uint32_t, 1>{1});
  appendAchievementTrack(definitions, index, AchievementMetric::FontsDownloaded,
                         std::array<uint32_t, 5>{1, 2, 5, 10, 20}, std::array<uint32_t, 1>{1});
  appendAchievementTrack(definitions, index, AchievementMetric::BooksImported,
                         std::array<uint32_t, 5>{1, 5, 10, 25, 50}, std::array<uint32_t, 1>{1});
  appendAchievementTrack(definitions, index, AchievementMetric::OtaUpdates, std::array<uint32_t, 5>{1, 2, 3, 5, 10},
                         std::array<uint32_t, 1>{1});
  appendAchievementTrack(definitions, index, AchievementMetric::DailyGoalsCompleted,
                         std::array<uint32_t, 9>{1, 3, 7, 14, 30, 60, 100, 180, 365}, std::array<uint32_t, 0>{});
  return definitions;
}

inline constexpr auto ACHIEVEMENT_DEFINITIONS = makeAchievementDefinitions();
static_assert(ACHIEVEMENT_DEFINITIONS[achievementCount() - 1].target == 365,
              "Achievement catalog count and generated tracks must stay in sync");

constexpr bool achievementBitIsSet(const AchievementBits& bits, const AchievementId id) {
  const size_t index = static_cast<size_t>(id);
  return index < achievementCount() && (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

inline void setAchievementBit(AchievementBits& bits, const AchievementId id) {
  const size_t index = static_cast<size_t>(id);
  if (index < achievementCount()) bits[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
}

inline void stampAchievementUnlockDays(AchievementUnlockDays& days, const AchievementBits& newlyUnlocked,
                                       const uint16_t dayIndex) {
  if (dayIndex == ACHIEVEMENT_DAY_UNKNOWN) return;
  for (size_t i = 0; i < achievementCount(); ++i) {
    if (achievementBitIsSet(newlyUnlocked, static_cast<AchievementId>(i)) && days[i] == ACHIEVEMENT_DAY_UNKNOWN) {
      days[i] = dayIndex;
    }
  }
}

inline uint16_t achievementPopcount(const AchievementBits& bits) {
  uint16_t count = 0;
  for (uint8_t value : bits) {
    while (value != 0) {
      value &= static_cast<uint8_t>(value - 1);
      ++count;
    }
  }
  return count;
}

inline uint32_t achievementMetricValue(const AchievementMetric metric, const AchievementSnapshot& snapshot) {
  switch (metric) {
    case AchievementMetric::Pages:
      return snapshot.pages;
    case AchievementMetric::ReadingSeconds:
      return snapshot.readingSeconds;
    case AchievementMetric::Sessions:
      return snapshot.sessions;
    case AchievementMetric::CompletedBooks:
      return snapshot.completedBooks;
    case AchievementMetric::LongestStreak:
      return snapshot.longestStreak;
    case AchievementMetric::NightSeconds:
      return snapshot.nightSeconds;
    case AchievementMetric::DictionaryLookups:
      return snapshot.interactions.dictionaryLookups;
    case AchievementMetric::BookmarksAdded:
      return snapshot.interactions.bookmarksAdded;
    case AchievementMetric::FormatsOpened: {
      uint8_t formats = snapshot.interactions.formatsOpened;
      uint32_t count = 0;
      while (formats != 0) {
        formats &= static_cast<uint8_t>(formats - 1);
        ++count;
      }
      return count;
    }
    case AchievementMetric::WifiConnections:
      return snapshot.interactions.wifiConnections;
    case AchievementMetric::FontsDownloaded:
      return snapshot.interactions.fontsDownloaded;
    case AchievementMetric::BooksImported:
      return snapshot.interactions.booksImported;
    case AchievementMetric::OtaUpdates:
      return snapshot.interactions.otaUpdates;
    case AchievementMetric::DailyGoalsCompleted:
      return snapshot.dailyGoalsCompleted;
  }
  return 0;
}

inline AchievementBits evaluateAchievementBits(const AchievementSnapshot& snapshot) {
  AchievementBits bits{};
  for (size_t i = 0; i < ACHIEVEMENT_DEFINITIONS.size(); ++i) {
    const auto& definition = ACHIEVEMENT_DEFINITIONS[i];
    if (achievementMetricValue(definition.metric, snapshot) >= definition.target) {
      setAchievementBit(bits, static_cast<AchievementId>(i));
    }
  }
  return bits;
}
