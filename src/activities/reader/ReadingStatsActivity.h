#pragma once

#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "achievements/AchievementModel.h"
#include "activities/Activity.h"
#include "reading_goal/ReadingGoalSystem.h"

class ReadingStatsActivity final : public Activity {
  enum class Page : uint8_t { Menu, Overview, Days, Weeks, Books, Habits, Goal, Achievements, AchievementDetail };

  struct BookRow {
    std::string title;
    std::string cachePath;
    BookReadingStats stats;
    uint8_t progressPercent = 0;
  };

  Page page = Page::Menu;
  int selectedIndex = 0;
  int selectedBook = 0;
  int selectedAchievement = 0;
  int selectedGoalSetting = 0;
  GlobalReadingStats stats;
  GlobalReadingStats displayStats;
  ReadingStatsDateTime today;
  bool hasClock = false;
  std::vector<BookRow> books;

  void openMenuItem();
  void loadBooks();
  void renderMenu();
  void renderOverview();
  void renderDays();
  void renderWeeks();
  void renderBooks();
  void renderHabits();
  void renderGoal();
  void renderAchievements();
  void renderAchievementDetail();

 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
