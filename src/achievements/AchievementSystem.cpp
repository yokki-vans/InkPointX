#include "AchievementSystem.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

#include "activities/reader/GlobalReadingStats.h"
#include "reading_goal/ReadingGoalSystem.h"

namespace {
constexpr char ACHIEVEMENT_PATH[] = "/.crosspoint/achievements.bin";
constexpr char ACHIEVEMENT_TEMP_PATH[] = "/.crosspoint/achievements.bin.tmp";
constexpr uint8_t FILE_VERSION = 2;
constexpr size_t FILE_SIZE = 64;
constexpr uint8_t LEGACY_FILE_VERSION = 1;
constexpr size_t LEGACY_FILE_SIZE = 44;
constexpr size_t UNLOCKED_OFFSET = 8;
constexpr size_t PENDING_OFFSET = UNLOCKED_OFFSET + ACHIEVEMENT_BIT_BYTES;
constexpr size_t COUNTERS_OFFSET = PENDING_OFFSET + ACHIEVEMENT_BIT_BYTES;
constexpr size_t CHECKSUM_OFFSET = 60;
constexpr std::array<uint8_t, 4> MAGIC = {'I', 'P', 'X', 'A'};

constexpr std::array<StrId, 16> LEGACY_NAMES = {
    StrId::STR_ACH_FIRST_PAGE,   StrId::STR_ACH_PAGE_TURNER,      StrId::STR_ACH_THOUSAND_PAGES,
    StrId::STR_ACH_QUIET_HOUR,   StrId::STR_ACH_DEEP_READER,      StrId::STR_ACH_REGULAR_READER,
    StrId::STR_ACH_FIRST_FINISH, StrId::STR_ACH_SEVEN_DAY_STREAK, StrId::STR_ACH_NIGHT_OWL,
    StrId::STR_ACH_WORD_HUNTER,  StrId::STR_ACH_BOOKMARK_KEEPER,  StrId::STR_ACH_FORMAT_EXPLORER,
    StrId::STR_ACH_CONNECTED,    StrId::STR_ACH_FONT_COLLECTOR,   StrId::STR_ACH_BOOK_COURIER,
    StrId::STR_ACH_UP_TO_DATE,
};

constexpr std::array<StrId, 16> LEGACY_DESCRIPTIONS = {
    StrId::STR_ACH_FIRST_PAGE_DESC,   StrId::STR_ACH_PAGE_TURNER_DESC,      StrId::STR_ACH_THOUSAND_PAGES_DESC,
    StrId::STR_ACH_QUIET_HOUR_DESC,   StrId::STR_ACH_DEEP_READER_DESC,      StrId::STR_ACH_REGULAR_READER_DESC,
    StrId::STR_ACH_FIRST_FINISH_DESC, StrId::STR_ACH_SEVEN_DAY_STREAK_DESC, StrId::STR_ACH_NIGHT_OWL_DESC,
    StrId::STR_ACH_WORD_HUNTER_DESC,  StrId::STR_ACH_BOOKMARK_KEEPER_DESC,  StrId::STR_ACH_FORMAT_EXPLORER_DESC,
    StrId::STR_ACH_CONNECTED_DESC,    StrId::STR_ACH_FONT_COLLECTOR_DESC,   StrId::STR_ACH_BOOK_COURIER_DESC,
    StrId::STR_ACH_UP_TO_DATE_DESC,
};

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe32(uint8_t* data, const size_t offset, const uint32_t value) {
  data[offset] = value & 0xff;
  data[offset + 1] = (value >> 8) & 0xff;
  data[offset + 2] = (value >> 16) & 0xff;
  data[offset + 3] = (value >> 24) & 0xff;
}

uint32_t checksum(const uint8_t* data, const size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t incrementSaturated(const uint32_t value) {
  return value == std::numeric_limits<uint32_t>::max() ? value : value + 1;
}

void copyBitsFromLegacy(AchievementBits& destination, const uint32_t legacy) {
  static_assert(achievementCount() >= 32);
  for (size_t i = 0; i < 32; ++i) {
    if ((legacy & (1u << i)) != 0) setAchievementBit(destination, static_cast<AchievementId>(i));
  }
}

bool addNewBits(AchievementBits& destination, const AchievementBits& source, AchievementBits* newlyAdded = nullptr) {
  bool changed = false;
  for (size_t i = 0; i < destination.size(); ++i) {
    const uint8_t added = source[i] & static_cast<uint8_t>(~destination[i]);
    if (newlyAdded) (*newlyAdded)[i] = added;
    if (added != 0) {
      destination[i] |= added;
      changed = true;
    }
  }
  return changed;
}

StrId trackNameId(const AchievementMetric metric) {
  switch (metric) {
    case AchievementMetric::Pages:
      return StrId::STR_ACH_TRACK_PAGES;
    case AchievementMetric::ReadingSeconds:
      return StrId::STR_ACH_TRACK_READING_TIME;
    case AchievementMetric::Sessions:
      return StrId::STR_ACH_TRACK_SESSIONS;
    case AchievementMetric::CompletedBooks:
      return StrId::STR_ACH_TRACK_BOOKS;
    case AchievementMetric::LongestStreak:
      return StrId::STR_ACH_TRACK_STREAK;
    case AchievementMetric::NightSeconds:
      return StrId::STR_ACH_TRACK_NIGHT;
    case AchievementMetric::DictionaryLookups:
      return StrId::STR_ACH_TRACK_DICTIONARY;
    case AchievementMetric::BookmarksAdded:
      return StrId::STR_ACH_TRACK_BOOKMARKS;
    case AchievementMetric::FormatsOpened:
      return StrId::STR_ACH_TRACK_FORMATS;
    case AchievementMetric::WifiConnections:
      return StrId::STR_ACH_TRACK_WIFI;
    case AchievementMetric::FontsDownloaded:
      return StrId::STR_ACH_TRACK_FONTS;
    case AchievementMetric::BooksImported:
      return StrId::STR_ACH_TRACK_TRANSFERS;
    case AchievementMetric::OtaUpdates:
      return StrId::STR_ACH_TRACK_UPDATES;
    case AchievementMetric::DailyGoalsCompleted:
      return StrId::STR_ACH_TRACK_DAILY_GOALS;
  }
  return StrId::STR_ACHIEVEMENTS;
}

std::string targetLabel(const AchievementDefinition& definition) {
  char value[32];
  if (definition.metric == AchievementMetric::ReadingSeconds || definition.metric == AchievementMetric::NightSeconds) {
    if (definition.target < 3600) {
      snprintf(value, sizeof(value), tr(STR_HOME_MINUTES_FORMAT), static_cast<unsigned long>(definition.target / 60));
    } else {
      snprintf(value, sizeof(value), tr(STR_HOME_HOURS_MINUTES_FORMAT),
               static_cast<unsigned long>(definition.target / 3600),
               static_cast<unsigned long>((definition.target % 3600) / 60));
    }
  } else {
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(definition.target));
  }
  return value;
}
}  // namespace

AchievementSystem& AchievementSystem::getInstance() {
  static AchievementSystem instance;
  return instance;
}

AchievementSnapshot AchievementSystem::makeSnapshot(const GlobalReadingStats& stats,
                                                    const bool includeDailyGoals) const {
  AchievementSnapshot snapshot;
  snapshot.pages = stats.totalPagesTurned;
  snapshot.readingSeconds = stats.totalReadingSeconds;
  snapshot.sessions = stats.totalSessions;
  snapshot.completedBooks = stats.completedBooks;
  snapshot.longestStreak = stats.displayLongestReadingStreak();
  snapshot.nightSeconds = stats.timeOfDaySeconds[static_cast<size_t>(ReadingTimeBucket::Night)];
  // Scanning the retained 730-day history is only needed for the daily-goal
  // track. Avoid doing it for every row while the 112-item list is scrolled.
  if (includeDailyGoals) snapshot.dailyGoalsCompleted = READING_GOAL.completedDays(stats);
  snapshot.interactions = counters;
  return snapshot;
}

bool AchievementSystem::ensureLoaded() {
  if (loaded) return true;
  if (!Storage.ready()) return false;
  loaded = true;
  bool valid = false;
  bool migrated = false;

  HalFile file;
  if (Storage.openFileForRead("ACH", ACHIEVEMENT_PATH, file)) {
    const size_t size = file.fileSize();
    std::array<uint8_t, FILE_SIZE> data{};
    const int read = file.read(data.data(), std::min(size, data.size()));
    file.close();
    if (size > FILE_SIZE || (read > 4 && data[4] > FILE_VERSION)) {
      blockSave = true;
      LOG_ERR("ACH", "Achievement file is newer (v%u, %u bytes); preserving it", data[4], static_cast<unsigned>(size));
      return true;
    }

    const bool current = size == FILE_SIZE && read == static_cast<int>(FILE_SIZE) &&
                         std::equal(MAGIC.begin(), MAGIC.end(), data.begin()) && data[4] == FILE_VERSION &&
                         readLe32(data.data(), CHECKSUM_OFFSET) == checksum(data.data(), CHECKSUM_OFFSET);
    const bool legacy = size == LEGACY_FILE_SIZE && read == static_cast<int>(LEGACY_FILE_SIZE) &&
                        std::equal(MAGIC.begin(), MAGIC.end(), data.begin()) && data[4] == LEGACY_FILE_VERSION &&
                        readLe32(data.data(), 40) == checksum(data.data(), 40);
    if (current) {
      counters.formatsOpened = data[5];
      std::copy_n(data.begin() + UNLOCKED_OFFSET, ACHIEVEMENT_BIT_BYTES, unlockedBits.begin());
      std::copy_n(data.begin() + PENDING_OFFSET, ACHIEVEMENT_BIT_BYTES, pendingBits.begin());
      counters.dictionaryLookups = readLe32(data.data(), COUNTERS_OFFSET);
      counters.bookmarksAdded = readLe32(data.data(), COUNTERS_OFFSET + 4);
      counters.booksImported = readLe32(data.data(), COUNTERS_OFFSET + 8);
      counters.wifiConnections = readLe32(data.data(), COUNTERS_OFFSET + 12);
      counters.fontsDownloaded = readLe32(data.data(), COUNTERS_OFFSET + 16);
      counters.otaUpdates = readLe32(data.data(), COUNTERS_OFFSET + 20);
      valid = true;
    } else if (legacy) {
      counters.formatsOpened = data[5];
      copyBitsFromLegacy(unlockedBits, readLe32(data.data(), 8));
      copyBitsFromLegacy(pendingBits, readLe32(data.data(), 12));
      counters.dictionaryLookups = readLe32(data.data(), 16);
      counters.bookmarksAdded = readLe32(data.data(), 20);
      counters.booksImported = readLe32(data.data(), 24);
      counters.wifiConnections = readLe32(data.data(), 28);
      counters.fontsDownloaded = readLe32(data.data(), 32);
      counters.otaUpdates = readLe32(data.data(), 36);
      valid = true;
      migrated = true;
    }
  }

  if (!valid) {
    counters = {};
    pendingBits = {};
    unlockedBits = evaluateAchievementBits(makeSnapshot(GlobalReadingStats::load()));
    save();
  } else if (migrated) {
    // Credit new history-based tiers silently while retaining any pending
    // v2.2.16 popup. Interaction counters continue from their saved values.
    addNewBits(unlockedBits, evaluateAchievementBits(makeSnapshot(GlobalReadingStats::load())));
    save();
  }
  return true;
}

bool AchievementSystem::save() const {
  if (!loaded || blockSave || !Storage.ready()) return false;
  Storage.mkdir("/.crosspoint");
  std::array<uint8_t, FILE_SIZE> data{};
  std::copy(MAGIC.begin(), MAGIC.end(), data.begin());
  data[4] = FILE_VERSION;
  data[5] = counters.formatsOpened;
  std::copy(unlockedBits.begin(), unlockedBits.end(), data.begin() + UNLOCKED_OFFSET);
  std::copy(pendingBits.begin(), pendingBits.end(), data.begin() + PENDING_OFFSET);
  writeLe32(data.data(), COUNTERS_OFFSET, counters.dictionaryLookups);
  writeLe32(data.data(), COUNTERS_OFFSET + 4, counters.bookmarksAdded);
  writeLe32(data.data(), COUNTERS_OFFSET + 8, counters.booksImported);
  writeLe32(data.data(), COUNTERS_OFFSET + 12, counters.wifiConnections);
  writeLe32(data.data(), COUNTERS_OFFSET + 16, counters.fontsDownloaded);
  writeLe32(data.data(), COUNTERS_OFFSET + 20, counters.otaUpdates);
  writeLe32(data.data(), CHECKSUM_OFFSET, checksum(data.data(), CHECKSUM_OFFSET));

  HalFile file;
  if (!Storage.openFileForWrite("ACH", ACHIEVEMENT_TEMP_PATH, file)) return false;
  const size_t written = file.write(data.data(), data.size());
  file.flush();
  const bool closed = file.close();
  if (written != data.size() || !closed || !Storage.replaceFileFromTemp(ACHIEVEMENT_PATH, ACHIEVEMENT_TEMP_PATH)) {
    Storage.remove(ACHIEVEMENT_TEMP_PATH);
    LOG_ERR("ACH", "Could not save achievement state");
    return false;
  }
  return true;
}

bool AchievementSystem::evaluate(const GlobalReadingStats& stats, const bool notify) {
  if (!ensureLoaded() || blockSave) return false;
  AchievementBits newlyUnlocked{};
  const bool changed = addNewBits(unlockedBits, evaluateAchievementBits(makeSnapshot(stats)), &newlyUnlocked);
  if (!changed) return false;
  if (notify) {
    for (size_t i = 0; i < pendingBits.size(); ++i) pendingBits[i] |= newlyUnlocked[i];
  }
  return true;
}

void AchievementSystem::refresh(const GlobalReadingStats& stats) {
  if (evaluate(stats, false)) save();
}

void AchievementSystem::record(const AchievementEvent event) {
  if (!ensureLoaded() || blockSave) return;
  switch (event) {
    case AchievementEvent::DictionaryLookup:
      counters.dictionaryLookups = incrementSaturated(counters.dictionaryLookups);
      break;
    case AchievementEvent::BookmarkAdded:
      counters.bookmarksAdded = incrementSaturated(counters.bookmarksAdded);
      break;
    case AchievementEvent::WifiConnected:
      counters.wifiConnections = incrementSaturated(counters.wifiConnections);
      break;
    case AchievementEvent::FontDownloaded:
      counters.fontsDownloaded = incrementSaturated(counters.fontsDownloaded);
      break;
    case AchievementEvent::BookImported:
      counters.booksImported = incrementSaturated(counters.booksImported);
      break;
    case AchievementEvent::OtaUpdated:
      counters.otaUpdates = incrementSaturated(counters.otaUpdates);
      break;
  }
  evaluate(GlobalReadingStats::load(), true);
  save();
}

void AchievementSystem::recordFormat(const AchievementBookFormat format) {
  if (!ensureLoaded() || blockSave || format >= AchievementBookFormat::Count) return;
  const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(format));
  if ((counters.formatsOpened & bit) != 0) return;
  counters.formatsOpened |= bit;
  evaluate(GlobalReadingStats::load(), true);
  save();
}

AchievementView AchievementSystem::view(const AchievementId id, const GlobalReadingStats& stats) {
  ensureLoaded();
  const size_t index = static_cast<size_t>(id);
  if (index >= ACHIEVEMENT_DEFINITIONS.size()) return {};
  const auto& definition = ACHIEVEMENT_DEFINITIONS[index];
  const bool needsDailyGoalHistory = definition.metric == AchievementMetric::DailyGoalsCompleted;
  const uint32_t current = achievementMetricValue(definition.metric, makeSnapshot(stats, needsDailyGoalHistory));
  return {name(id), description(id), std::min(current, definition.target), definition.target,
          achievementBitIsSet(unlockedBits, id)};
}

uint16_t AchievementSystem::unlockedCount(const GlobalReadingStats& stats) {
  refresh(stats);
  return achievementPopcount(unlockedBits);
}

bool AchievementSystem::takePendingUnlocks(AchievementId& first, uint16_t& count) {
  if (!ensureLoaded()) return false;
  count = achievementPopcount(pendingBits);
  if (count == 0) return false;
  for (size_t i = 0; i < achievementCount(); ++i) {
    const auto candidate = static_cast<AchievementId>(i);
    if (achievementBitIsSet(pendingBits, candidate)) {
      first = candidate;
      break;
    }
  }
  pendingBits = {};
  save();
  return true;
}

std::string AchievementSystem::name(const AchievementId id) const {
  const size_t index = static_cast<size_t>(id);
  if (index < LEGACY_NAMES.size()) return I18N.get(LEGACY_NAMES[index]);
  if (index >= ACHIEVEMENT_DEFINITIONS.size()) return {};
  const auto& definition = ACHIEVEMENT_DEFINITIONS[index];
  return std::string(I18N.get(trackNameId(definition.metric))) + " · " + targetLabel(definition);
}

std::string AchievementSystem::description(const AchievementId id) const {
  const size_t index = static_cast<size_t>(id);
  if (index < LEGACY_DESCRIPTIONS.size()) return I18N.get(LEGACY_DESCRIPTIONS[index]);
  return index < ACHIEVEMENT_DEFINITIONS.size() ? I18N.get(StrId::STR_ACH_MILESTONE_DESC) : "";
}
