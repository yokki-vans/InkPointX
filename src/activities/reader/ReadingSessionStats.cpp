#include "ReadingSessionStats.h"

#include <Arduino.h>

#include <algorithm>

#include "achievements/AchievementSystem.h"
#include "reading_goal/ReadingGoalSystem.h"

namespace {
constexpr unsigned long MIN_PAGE_MS = 2000UL;
constexpr uint32_t MIN_SESSION_SECONDS_FOR_TIME = 10UL;
constexpr uint32_t MIN_SESSION_SECONDS_FOR_COUNT = 60UL;

uint32_t addSaturated(const uint32_t left, const uint32_t right) {
  return UINT32_MAX - left < right ? UINT32_MAX : left + right;
}
}  // namespace

void ReadingSessionStats::begin(const std::string& cachePath) {
  book = BookReadingStats::load(cachePath);
  global = GlobalReadingStats::load();
  sessionSeconds = 0;
  pageShownAtMs = 0;
  hasSessionStart = getCurrentLocalReadingStatsDateTime(sessionStart);
  started = true;
}

void ReadingSessionStats::pageShown() {
  if (started) pageShownAtMs = millis();
}

void ReadingSessionStats::collectCurrentPage(const bool forwardPage) {
  if (!started || pageShownAtMs == 0) return;
  const unsigned long elapsed = millis() - pageShownAtMs;
  pageShownAtMs = 0;
  if (elapsed < MIN_PAGE_MS) return;
  const uint32_t seconds = static_cast<uint32_t>(elapsed / 1000UL);
  sessionSeconds = addSaturated(sessionSeconds, seconds);
  if (forwardPage) {
    book.recordForwardPageRead(seconds);
    book.totalPagesTurned = addSaturated(book.totalPagesTurned, 1);
    global.totalPagesTurned = addSaturated(global.totalPagesTurned, 1);
  }
}

void ReadingSessionStats::pageTurn(const bool forwardPage) { collectCurrentPage(forwardPage); }

void ReadingSessionStats::finish(const std::string& cachePath) {
  if (!started) return;
  collectCurrentPage(false);
  if (sessionSeconds >= MIN_SESSION_SECONDS_FOR_TIME) {
    book.totalReadingSeconds = addSaturated(book.totalReadingSeconds, sessionSeconds);
    global.totalReadingSeconds = addSaturated(global.totalReadingSeconds, sessionSeconds);
    if (sessionSeconds >= MIN_SESSION_SECONDS_FOR_COUNT) {
      if (book.sessionCount < UINT16_MAX) ++book.sessionCount;
      global.totalSessions = addSaturated(global.totalSessions, 1);
    }
    if (hasSessionStart) {
      book.recordReadingSpan(sessionStart, sessionSeconds);
      global.recordReadingSpan(sessionStart, sessionSeconds);
      if (!book.startDateManual && !book.startDate.isValid() && sessionSeconds >= 120) {
        book.startDate = sessionStart.date;
      }
    }
  }
  book.save(cachePath);
  global.save();
  READING_GOAL.onStatsUpdated(global);
  ACHIEVEMENTS.refresh(global);
  started = false;
}
