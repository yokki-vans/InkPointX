#include "AchievementVisuals.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "components/icons/lucide_ui.h"

UIIcon achievementIcon(const AchievementId id) {
  const size_t index = static_cast<size_t>(id);
  if (index >= ACHIEVEMENT_DEFINITIONS.size()) return UIIcon::Favorite;
  switch (ACHIEVEMENT_DEFINITIONS[index].metric) {
    case AchievementMetric::Pages:
      return UIIcon::ReaderPage;
    case AchievementMetric::ReadingSeconds:
    case AchievementMetric::NightSeconds:
      return UIIcon::Clock;
    case AchievementMetric::Sessions:
      return UIIcon::Recent;
    case AchievementMetric::CompletedBooks:
      return UIIcon::Book;
    case AchievementMetric::LongestStreak:
    case AchievementMetric::DailyGoalsCompleted:
      return UIIcon::Favorite;
    case AchievementMetric::DictionaryLookups:
      return UIIcon::ReaderDictionary;
    case AchievementMetric::BookmarksAdded:
      return UIIcon::Bookmark;
    case AchievementMetric::FormatsOpened:
      return UIIcon::Files;
    case AchievementMetric::WifiConnections:
      return UIIcon::Wifi;
    case AchievementMetric::FontsDownloaded:
      return UIIcon::Interface;
    case AchievementMetric::BooksImported:
      return UIIcon::Transfer;
    case AchievementMetric::OtaUpdates:
      return UIIcon::System;
  }
  return UIIcon::Favorite;
}

const uint8_t* achievementIconBitmap(const AchievementId id) {
  switch (achievementIcon(id)) {
    case UIIcon::ReaderPage:
      return LucidePage32;
    case UIIcon::Clock:
    case UIIcon::Recent:
      return LucideClock32;
    case UIIcon::Book:
      return LucideBookOpen32;
    case UIIcon::Favorite:
      return LucideStar32;
    case UIIcon::ReaderDictionary:
      return LucideDictionary32;
    case UIIcon::Bookmark:
      return LucideBookmark32;
    case UIIcon::Files:
      return LucideFiles32;
    case UIIcon::Wifi:
      return LucideWifi32;
    case UIIcon::Interface:
      return LucideInterface32;
    case UIIcon::Transfer:
      return LucideSend32;
    case UIIcon::System:
      return LucideSystem32;
    default:
      return LucideStar32;
  }
}

void drawAchievementMedallion(const GfxRenderer& renderer, const AchievementId id, const int x, const int y,
                              const int size, const bool unlocked) {
  const int radius = std::max(10, size / 4);
  renderer.fillRoundedRect(x, y, size, size, radius, Color::White);
  renderer.drawRoundedRect(x, y, size, size, unlocked ? 2 : 1, radius, true);

  if (const uint8_t* icon = achievementIconBitmap(id)) {
    renderer.drawIcon(icon, x + (size - 32) / 2, y + (size - 32) / 2, 32, 32);
  }
}
