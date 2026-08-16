#pragma once

#include <cstdint>

#include "AchievementModel.h"

struct GlobalReadingStats;

struct AchievementView {
  const char* name = "";
  const char* description = "";
  uint32_t current = 0;
  uint32_t target = 0;
  bool unlocked = false;
};

class AchievementSystem {
  AchievementCounters counters;
  uint32_t unlockedMask = 0;
  uint32_t pendingMask = 0;
  bool loaded = false;
  bool blockSave = false;

  bool ensureLoaded();
  bool save() const;
  AchievementSnapshot makeSnapshot(const GlobalReadingStats& stats) const;
  bool evaluate(const GlobalReadingStats& stats, bool notify);

 public:
  static AchievementSystem& getInstance();

  void refresh(const GlobalReadingStats& stats);
  void record(AchievementEvent event);
  void recordFormat(AchievementBookFormat format);

  AchievementView view(AchievementId id, const GlobalReadingStats& stats);
  uint8_t unlockedCount(const GlobalReadingStats& stats);
  uint32_t takePendingUnlocks();

  const char* name(AchievementId id) const;
  const char* description(AchievementId id) const;
};

#define ACHIEVEMENTS AchievementSystem::getInstance()
