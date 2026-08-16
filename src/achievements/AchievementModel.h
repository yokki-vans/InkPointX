#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
  Count,
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
  AchievementCounters interactions;
};

struct AchievementDefinition {
  AchievementMetric metric;
  uint32_t target;
};

inline constexpr std::array<AchievementDefinition, static_cast<size_t>(AchievementId::Count)>
    ACHIEVEMENT_DEFINITIONS = {{{AchievementMetric::Pages, 1},
                                {AchievementMetric::Pages, 100},
                                {AchievementMetric::Pages, 1000},
                                {AchievementMetric::ReadingSeconds, 3600},
                                {AchievementMetric::ReadingSeconds, 36000},
                                {AchievementMetric::Sessions, 10},
                                {AchievementMetric::CompletedBooks, 1},
                                {AchievementMetric::LongestStreak, 7},
                                {AchievementMetric::NightSeconds, 3600},
                                {AchievementMetric::DictionaryLookups, 10},
                                {AchievementMetric::BookmarksAdded, 10},
                                {AchievementMetric::FormatsOpened, 3},
                                {AchievementMetric::WifiConnections, 1},
                                {AchievementMetric::FontsDownloaded, 1},
                                {AchievementMetric::BooksImported, 1},
                                {AchievementMetric::OtaUpdates, 1}}};

constexpr size_t achievementCount() { return static_cast<size_t>(AchievementId::Count); }

constexpr uint32_t achievementBit(const AchievementId id) {
  return 1u << static_cast<uint8_t>(id);
}

inline uint8_t achievementPopcount(uint32_t value) {
  uint8_t count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
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
    case AchievementMetric::FormatsOpened:
      return achievementPopcount(snapshot.interactions.formatsOpened);
    case AchievementMetric::WifiConnections:
      return snapshot.interactions.wifiConnections;
    case AchievementMetric::FontsDownloaded:
      return snapshot.interactions.fontsDownloaded;
    case AchievementMetric::BooksImported:
      return snapshot.interactions.booksImported;
    case AchievementMetric::OtaUpdates:
      return snapshot.interactions.otaUpdates;
  }
  return 0;
}

inline uint32_t evaluateAchievementMask(const AchievementSnapshot& snapshot) {
  uint32_t mask = 0;
  for (size_t i = 0; i < ACHIEVEMENT_DEFINITIONS.size(); ++i) {
    const auto& definition = ACHIEVEMENT_DEFINITIONS[i];
    if (achievementMetricValue(definition.metric, snapshot) >= definition.target) {
      mask |= achievementBit(static_cast<AchievementId>(i));
    }
  }
  return mask;
}
