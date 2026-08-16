#include "ReadingGoalSystem.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>

namespace {
constexpr char GOAL_PATH[] = "/.crosspoint/reading_goal.bin";
constexpr char GOAL_TEMP_PATH[] = "/.crosspoint/reading_goal.bin.tmp";
constexpr std::array<uint8_t, 4> MAGIC = {'I', 'P', 'X', 'G'};
constexpr uint8_t FILE_VERSION = 1;
constexpr size_t FILE_SIZE = 24;
constexpr uint8_t FLAG_ENABLED = 1u << 0;
constexpr uint8_t FLAG_REMINDERS = 1u << 1;
constexpr uint16_t MIN_GOAL_MINUTES = 5;
constexpr uint16_t MAX_GOAL_MINUTES = 240;
constexpr unsigned long REMINDER_POLL_MS = 15UL * 60UL * 1000UL;

uint16_t readLe16(const uint8_t* data, const size_t offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void writeLe16(uint8_t* data, const size_t offset, const uint16_t value) {
  data[offset] = value & 0xff;
  data[offset + 1] = (value >> 8) & 0xff;
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
}  // namespace

ReadingGoalSystem& ReadingGoalSystem::getInstance() {
  static ReadingGoalSystem instance;
  return instance;
}

bool ReadingGoalSystem::ensureLoaded() {
  if (loaded) return true;
  if (!Storage.ready()) return false;
  loaded = true;

  HalFile file;
  if (!Storage.openFileForRead("GOAL", GOAL_PATH, file)) return true;
  const size_t size = file.fileSize();
  std::array<uint8_t, FILE_SIZE> data{};
  const int read = file.read(data.data(), data.size());
  file.close();

  if (size > FILE_SIZE || (read > 4 && data[4] > FILE_VERSION)) {
    blockSave = true;
    LOG_ERR("GOAL", "Goal settings file is newer; preserving it");
    return true;
  }
  const bool valid = size == FILE_SIZE && read == static_cast<int>(FILE_SIZE) &&
                     std::equal(MAGIC.begin(), MAGIC.end(), data.begin()) && data[4] == FILE_VERSION &&
                     readLe32(data.data(), 20) == checksum(data.data(), 20);
  if (!valid) {
    LOG_ERR("GOAL", "Ignoring invalid goal settings");
    return true;
  }

  const uint8_t flags = data[5];
  config.enabled = (flags & FLAG_ENABLED) != 0;
  config.remindersEnabled = (flags & FLAG_REMINDERS) != 0;
  config.dailyMinutes = std::clamp(readLe16(data.data(), 6), MIN_GOAL_MINUTES, MAX_GOAL_MINUTES);
  config.reminderHour = std::min<uint8_t>(data[8], 23);
  if (data[9] == static_cast<uint8_t>(ReadingGoalNotificationType::Completed)) {
    pending.type = ReadingGoalNotificationType::Completed;
    pending.currentMinutes = readLe16(data.data(), 10);
    pending.targetMinutes = config.dailyMinutes;
  }
  lastReminderDay = readLe32(data.data(), 12);
  lastCompletionDay = readLe32(data.data(), 16);
  return true;
}

bool ReadingGoalSystem::save() const {
  if (!loaded || blockSave || !Storage.ready()) return false;
  Storage.mkdir("/.crosspoint");
  std::array<uint8_t, FILE_SIZE> data{};
  std::copy(MAGIC.begin(), MAGIC.end(), data.begin());
  data[4] = FILE_VERSION;
  data[5] = (config.enabled ? FLAG_ENABLED : 0) | (config.remindersEnabled ? FLAG_REMINDERS : 0);
  writeLe16(data.data(), 6, config.dailyMinutes);
  data[8] = config.reminderHour;
  data[9] = static_cast<uint8_t>(pending.type);
  writeLe16(data.data(), 10, pending.currentMinutes);
  writeLe32(data.data(), 12, lastReminderDay);
  writeLe32(data.data(), 16, lastCompletionDay);
  writeLe32(data.data(), 20, checksum(data.data(), 20));

  HalFile file;
  if (!Storage.openFileForWrite("GOAL", GOAL_TEMP_PATH, file)) return false;
  const size_t written = file.write(data.data(), data.size());
  file.flush();
  const bool closed = file.close();
  if (written != data.size() || !closed || !Storage.replaceFileFromTemp(GOAL_PATH, GOAL_TEMP_PATH)) {
    Storage.remove(GOAL_TEMP_PATH);
    LOG_ERR("GOAL", "Could not save reading goal settings");
    return false;
  }
  return true;
}

ReadingGoalSettings ReadingGoalSystem::settings() {
  ensureLoaded();
  return config;
}

void ReadingGoalSystem::setEnabled(const bool enabled) {
  if (!ensureLoaded() || blockSave || config.enabled == enabled) return;
  config.enabled = enabled;
  pending = {};
  save();
}

void ReadingGoalSystem::setDailyMinutes(const uint16_t minutes) {
  if (!ensureLoaded() || blockSave) return;
  const uint16_t clamped = std::clamp(minutes, MIN_GOAL_MINUTES, MAX_GOAL_MINUTES);
  if (config.dailyMinutes == clamped) return;
  config.dailyMinutes = clamped;
  lastCompletionDay = 0;
  pending = {};
  save();
}

void ReadingGoalSystem::setRemindersEnabled(const bool enabled) {
  if (!ensureLoaded() || blockSave || config.remindersEnabled == enabled) return;
  config.remindersEnabled = enabled;
  pending = {};
  save();
}

bool ReadingGoalSystem::dayCompleted(const GlobalReadingStats& stats, const ReadingStatsDate& date,
                                     const uint32_t targetSeconds) {
  return date.isValid() && targetSeconds > 0 && stats.readingSecondsForDay(date) >= targetSeconds;
}

ReadingGoalProgress ReadingGoalSystem::progress(const GlobalReadingStats& stats, const ReadingStatsDateTime* now) {
  ensureLoaded();
  ReadingGoalProgress result;
  result.targetSeconds = static_cast<uint32_t>(config.dailyMinutes) * 60u;
  if (!config.enabled || result.targetSeconds == 0) return result;

  ReadingStatsDateTime current;
  if (now) {
    current = *now;
  } else if (!getCurrentLocalReadingStatsDateTime(current)) {
    return result;
  }
  result.todaySeconds = stats.readingSecondsForDay(current.date);

  ReadingStatsDate cursor = current.date;
  for (uint16_t offset = 0; offset < READING_HISTORY_DAYS; ++offset) {
    const bool completed = dayCompleted(stats, cursor, result.targetSeconds);
    if (offset < 7 && completed) ++result.completedLast7Days;
    if (offset < 30 && completed) ++result.completedLast30Days;
    if (completed) ++result.completedAllTime;
    addDaysToReadingStatsDate(cursor, -1);
  }

  cursor = current.date;
  if (!dayCompleted(stats, cursor, result.targetSeconds)) addDaysToReadingStatsDate(cursor, -1);
  while (result.currentStreak < READING_HISTORY_DAYS && dayCompleted(stats, cursor, result.targetSeconds)) {
    ++result.currentStreak;
    addDaysToReadingStatsDate(cursor, -1);
  }
  return result;
}

uint16_t ReadingGoalSystem::completedDays(const GlobalReadingStats& stats, const ReadingStatsDateTime* now) {
  return progress(stats, now).completedAllTime;
}

void ReadingGoalSystem::onStatsUpdated(const GlobalReadingStats& stats) {
  if (!ensureLoaded() || blockSave || !config.enabled) return;
  ReadingStatsDateTime current;
  if (!getCurrentLocalReadingStatsDateTime(current)) return;
  const uint32_t day = readingStatsDayIndex(current.date);
  const uint32_t targetSeconds = static_cast<uint32_t>(config.dailyMinutes) * 60u;
  if (stats.readingSecondsForDay(current.date) < targetSeconds || lastCompletionDay == day) return;

  lastCompletionDay = day;
  pending.type = ReadingGoalNotificationType::Completed;
  pending.currentMinutes = static_cast<uint16_t>(stats.readingSecondsForDay(current.date) / 60u);
  pending.targetMinutes = config.dailyMinutes;
  pending.remainingMinutes = 0;
  save();
}

bool ReadingGoalSystem::takeNotification(ReadingGoalNotification& notification) {
  if (!ensureLoaded() || !config.enabled) return false;
  if (pending.type != ReadingGoalNotificationType::None) {
    notification = pending;
    pending = {};
    save();
    return true;
  }
  if (!config.remindersEnabled) return false;

  const unsigned long nowMs = millis();
  if (lastReminderPollMs != 0 && nowMs - lastReminderPollMs < REMINDER_POLL_MS) return false;
  lastReminderPollMs = nowMs;

  ReadingStatsDateTime current;
  if (!getCurrentLocalReadingStatsDateTime(current) || current.hour < config.reminderHour) return false;
  const uint32_t day = readingStatsDayIndex(current.date);
  if (day == lastReminderDay || day == lastCompletionDay) return false;

  const GlobalReadingStats stats = GlobalReadingStats::load();
  const uint32_t currentSeconds = stats.readingSecondsForDay(current.date);
  const uint32_t targetSeconds = static_cast<uint32_t>(config.dailyMinutes) * 60u;
  if (currentSeconds >= targetSeconds) return false;

  const uint32_t remainingSeconds = targetSeconds - currentSeconds;
  notification.type = ReadingGoalNotificationType::Reminder;
  notification.currentMinutes = static_cast<uint16_t>(currentSeconds / 60u);
  notification.targetMinutes = config.dailyMinutes;
  notification.remainingMinutes = static_cast<uint16_t>((remainingSeconds + 59u) / 60u);
  lastReminderDay = day;
  save();
  return true;
}
