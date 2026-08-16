#pragma once

#include <cstdint>
#include <string>

#include "AchievementModel.h"

struct GlobalReadingStats;

struct AchievementView {
  std::string name;
  std::string description;
  uint32_t current = 0;
  uint32_t target = 0;
  uint16_t earnedDayIndex = ACHIEVEMENT_DAY_UNKNOWN;
  bool unlocked = false;
};

class AchievementSystem {
  AchievementCounters counters;
  AchievementBits unlockedBits{};
  AchievementBits pendingBits{};
  AchievementUnlockDays unlockDays = makeUnknownAchievementUnlockDays();
  bool loaded = false;
  bool blockSave = false;

  bool ensureLoaded();
  bool save() const;
  AchievementSnapshot makeSnapshot(const GlobalReadingStats& stats, bool includeDailyGoals = true) const;
  bool evaluate(const GlobalReadingStats& stats, bool notify);

 public:
  static AchievementSystem& getInstance();

  void refresh(const GlobalReadingStats& stats);
  void record(AchievementEvent event);
  void recordFormat(AchievementBookFormat format);

  AchievementView view(AchievementId id, const GlobalReadingStats& stats);
  bool isUnlocked(AchievementId id);
  uint16_t unlockedCount(const GlobalReadingStats& stats);
  bool takePendingUnlocks(AchievementId& first, uint16_t& count);

  std::string name(AchievementId id) const;
  std::string description(AchievementId id) const;
};

#define ACHIEVEMENTS AchievementSystem::getInstance()
