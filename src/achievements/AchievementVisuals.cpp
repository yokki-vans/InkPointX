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
  const int shadowOffset = std::max(2, size / 24);
  renderer.fillRoundedRect(x + shadowOffset, y + shadowOffset, size, size, radius, Color::LightGray);
  renderer.fillRoundedRect(x, y, size, size, radius, Color::White);
  renderer.drawRoundedRect(x, y, size, size, 1, radius, true);
  renderer.drawRoundedRect(x + 5, y + 5, size - 10, size - 10, 1, std::max(7, radius - 4), true);

  // Restrained corner marks make the badge feel engraved while remaining
  // clean on 1-bit panels and cheap to refresh.
  const int mark = std::max(5, size / 11);
  renderer.drawLine(x + 11, y + 11, x + 11 + mark, y + 11, true);
  renderer.drawLine(x + 11, y + 11, x + 11, y + 11 + mark, true);
  renderer.drawLine(x + size - 12 - mark, y + size - 12, x + size - 12, y + size - 12, true);
  renderer.drawLine(x + size - 12, y + size - 12 - mark, x + size - 12, y + size - 12, true);

  if (const uint8_t* icon = achievementIconBitmap(id)) {
    renderer.drawIcon(icon, x + (size - 32) / 2, y + (size - 32) / 2, 32, 32);
  }

  if (unlocked) {
    const int sealW = std::max(28, size / 2);
    const int sealH = std::max(5, size / 13);
    renderer.fillRoundedRect(x + (size - sealW) / 2, y + size - 10, sealW, sealH, sealH / 2, Color::Black);
  } else {
    // A sparse veil distinguishes locked badges without erasing the icon.
    for (int py = y + 8; py < y + size - 8; py += 4) {
      const int offset = ((py - y) / 4) % 2 == 0 ? 0 : 2;
      for (int px = x + 8 + offset; px < x + size - 8; px += 4) renderer.drawPixel(px, py, false);
    }
  }
}
