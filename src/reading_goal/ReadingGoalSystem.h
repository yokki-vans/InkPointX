#pragma once

#include <cstdint>

#include "activities/reader/GlobalReadingStats.h"

struct ReadingGoalSettings {
  uint16_t dailyMinutes = 30;
  uint8_t reminderHour = 20;
  bool enabled = false;
  bool remindersEnabled = true;
};

struct ReadingGoalProgress {
  uint32_t todaySeconds = 0;
  uint32_t targetSeconds = 0;
  uint16_t currentStreak = 0;
  uint16_t completedLast7Days = 0;
  uint16_t completedLast30Days = 0;
  uint16_t completedAllTime = 0;
};

enum class ReadingGoalNotificationType : uint8_t { None = 0, Completed, Reminder };

struct ReadingGoalNotification {
  ReadingGoalNotificationType type = ReadingGoalNotificationType::None;
  uint16_t currentMinutes = 0;
  uint16_t targetMinutes = 0;
  uint16_t remainingMinutes = 0;
};

class ReadingGoalSystem {
  ReadingGoalSettings config;
  uint32_t lastReminderDay = 0;
  uint32_t lastCompletionDay = 0;
  ReadingGoalNotification pending;
  unsigned long lastReminderPollMs = 0;
  bool loaded = false;
  bool blockSave = false;

  bool ensureLoaded();
  bool save() const;
  static bool dayCompleted(const GlobalReadingStats& stats, const ReadingStatsDate& date, uint32_t targetSeconds);

 public:
  static ReadingGoalSystem& getInstance();

  ReadingGoalSettings settings();
  void setEnabled(bool enabled);
  void setDailyMinutes(uint16_t minutes);
  void setRemindersEnabled(bool enabled);

  ReadingGoalProgress progress(const GlobalReadingStats& stats, const ReadingStatsDateTime* now = nullptr);
  uint16_t completedDays(const GlobalReadingStats& stats, const ReadingStatsDateTime* now = nullptr);

  // Called immediately after a reader persists a session. Completion notices
  // are queued in RAM and shown after returning to a non-reader screen.
  void onStatsUpdated(const GlobalReadingStats& stats);
  bool takeNotification(ReadingGoalNotification& notification);
};

#define READING_GOAL ReadingGoalSystem::getInstance()
