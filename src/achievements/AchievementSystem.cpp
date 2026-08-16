#include "AchievementSystem.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "activities/reader/GlobalReadingStats.h"

namespace {
constexpr char ACHIEVEMENT_PATH[] = "/.crosspoint/achievements.bin";
constexpr char ACHIEVEMENT_TEMP_PATH[] = "/.crosspoint/achievements.bin.tmp";
constexpr uint8_t FILE_VERSION = 1;
constexpr size_t FILE_SIZE = 44;
constexpr std::array<uint8_t, 4> MAGIC = {'I', 'P', 'X', 'A'};

constexpr std::array<StrId, achievementCount()> NAMES = {
    StrId::STR_ACH_FIRST_PAGE,   StrId::STR_ACH_PAGE_TURNER,      StrId::STR_ACH_THOUSAND_PAGES,
    StrId::STR_ACH_QUIET_HOUR,   StrId::STR_ACH_DEEP_READER,      StrId::STR_ACH_REGULAR_READER,
    StrId::STR_ACH_FIRST_FINISH, StrId::STR_ACH_SEVEN_DAY_STREAK, StrId::STR_ACH_NIGHT_OWL,
    StrId::STR_ACH_WORD_HUNTER,  StrId::STR_ACH_BOOKMARK_KEEPER,  StrId::STR_ACH_FORMAT_EXPLORER,
    StrId::STR_ACH_CONNECTED,    StrId::STR_ACH_FONT_COLLECTOR,   StrId::STR_ACH_BOOK_COURIER,
    StrId::STR_ACH_UP_TO_DATE,
};

constexpr std::array<StrId, achievementCount()> DESCRIPTIONS = {
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
}  // namespace

AchievementSystem& AchievementSystem::getInstance() {
  static AchievementSystem instance;
  return instance;
}

AchievementSnapshot AchievementSystem::makeSnapshot(const GlobalReadingStats& stats) const {
  AchievementSnapshot snapshot;
  snapshot.pages = stats.totalPagesTurned;
  snapshot.readingSeconds = stats.totalReadingSeconds;
  snapshot.sessions = stats.totalSessions;
  snapshot.completedBooks = stats.completedBooks;
  snapshot.longestStreak = stats.displayLongestReadingStreak();
  snapshot.nightSeconds = stats.timeOfDaySeconds[static_cast<size_t>(ReadingTimeBucket::Night)];
  snapshot.interactions = counters;
  return snapshot;
}

bool AchievementSystem::ensureLoaded() {
  if (loaded) return true;
  if (!Storage.ready()) return false;

  loaded = true;
  bool valid = false;
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
    valid = size == FILE_SIZE && read == static_cast<int>(FILE_SIZE) &&
            std::equal(MAGIC.begin(), MAGIC.end(), data.begin()) && data[4] == FILE_VERSION &&
            readLe32(data.data(), 40) == checksum(data.data(), 40);
    if (valid) {
      counters.formatsOpened = data[5];
      unlockedMask = readLe32(data.data(), 8);
      pendingMask = readLe32(data.data(), 12);
      counters.dictionaryLookups = readLe32(data.data(), 16);
      counters.bookmarksAdded = readLe32(data.data(), 20);
      counters.booksImported = readLe32(data.data(), 24);
      counters.wifiConnections = readLe32(data.data(), 28);
      counters.fontsDownloaded = readLe32(data.data(), 32);
      counters.otaUpdates = readLe32(data.data(), 36);
    }
  }

  if (!valid) {
    // First install (or a damaged tiny state file): credit existing reading
    // history silently. Users keep their progress without receiving a stack of
    // old unlock popups immediately after updating.
    counters = {};
    pendingMask = 0;
    unlockedMask = evaluateAchievementMask(makeSnapshot(GlobalReadingStats::load()));
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
  writeLe32(data.data(), 8, unlockedMask);
  writeLe32(data.data(), 12, pendingMask);
  writeLe32(data.data(), 16, counters.dictionaryLookups);
  writeLe32(data.data(), 20, counters.bookmarksAdded);
  writeLe32(data.data(), 24, counters.booksImported);
  writeLe32(data.data(), 28, counters.wifiConnections);
  writeLe32(data.data(), 32, counters.fontsDownloaded);
  writeLe32(data.data(), 36, counters.otaUpdates);
  writeLe32(data.data(), 40, checksum(data.data(), 40));

  HalFile file;
  if (!Storage.openFileForWrite("ACH", ACHIEVEMENT_TEMP_PATH, file)) return false;
  const size_t written = file.write(data.data(), data.size());
  file.flush();
  const bool closed = file.close();
  if (written != data.size() || !closed) {
    Storage.remove(ACHIEVEMENT_TEMP_PATH);
    LOG_ERR("ACH", "Could not write achievement state");
    return false;
  }
  if (!Storage.replaceFileFromTemp(ACHIEVEMENT_PATH, ACHIEVEMENT_TEMP_PATH)) {
    Storage.remove(ACHIEVEMENT_TEMP_PATH);
    LOG_ERR("ACH", "Could not commit achievement state");
    return false;
  }
  return true;
}

bool AchievementSystem::evaluate(const GlobalReadingStats& stats, const bool notify) {
  if (!ensureLoaded() || blockSave) return false;
  const uint32_t earned = evaluateAchievementMask(makeSnapshot(stats));
  const uint32_t newlyUnlocked = earned & ~unlockedMask;
  if (newlyUnlocked == 0) return false;
  unlockedMask |= newlyUnlocked;
  if (notify) pendingMask |= newlyUnlocked;
  return true;
}

void AchievementSystem::refresh(const GlobalReadingStats& stats) {
  if (evaluate(stats, false)) save();
}

void AchievementSystem::record(const AchievementEvent event) {
  if (!ensureLoaded() || blockSave) return;
  switch (event) {
    case AchievementEvent::DictionaryLookup:
      if (counters.dictionaryLookups >= 10) return;
      counters.dictionaryLookups = incrementSaturated(counters.dictionaryLookups);
      break;
    case AchievementEvent::BookmarkAdded:
      if (counters.bookmarksAdded >= 10) return;
      counters.bookmarksAdded = incrementSaturated(counters.bookmarksAdded);
      break;
    case AchievementEvent::WifiConnected:
      if (counters.wifiConnections >= 1) return;
      counters.wifiConnections = 1;
      break;
    case AchievementEvent::FontDownloaded:
      if (counters.fontsDownloaded >= 1) return;
      counters.fontsDownloaded = 1;
      break;
    case AchievementEvent::BookImported:
      if (counters.booksImported >= 1) return;
      counters.booksImported = 1;
      break;
    case AchievementEvent::OtaUpdated:
      if (counters.otaUpdates >= 1) return;
      counters.otaUpdates = 1;
      break;
  }
  evaluate(GlobalReadingStats::load(), true);
  // Counters that have not reached a threshold still need persistence.
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
  const uint32_t current = achievementMetricValue(definition.metric, makeSnapshot(stats));
  return {name(id), description(id), std::min(current, definition.target), definition.target,
          (unlockedMask & achievementBit(id)) != 0};
}

uint8_t AchievementSystem::unlockedCount(const GlobalReadingStats& stats) {
  refresh(stats);
  return achievementPopcount(unlockedMask);
}

uint32_t AchievementSystem::takePendingUnlocks() {
  if (!ensureLoaded() || pendingMask == 0) return 0;
  const uint32_t result = pendingMask;
  pendingMask = 0;
  save();
  return result;
}

const char* AchievementSystem::name(const AchievementId id) const {
  const size_t index = static_cast<size_t>(id);
  return index < NAMES.size() ? I18N.get(NAMES[index]) : "";
}

const char* AchievementSystem::description(const AchievementId id) const {
  const size_t index = static_cast<size_t>(id);
  return index < DESCRIPTIONS.size() ? I18N.get(DESCRIPTIONS[index]) : "";
}
