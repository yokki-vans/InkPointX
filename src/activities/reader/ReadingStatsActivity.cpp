#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <numeric>

#include "BookStatsActivity.h"
#include "MappedInputManager.h"
#include "ReadingStatsSummary.h"
#include "RecentBooksStore.h"
#include "achievements/AchievementSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr int PAGE_MARGIN = 20;
constexpr int CARD_GAP = 12;
constexpr int CARD_RADIUS = 12;

constexpr std::array<StrId, 7> DAY_LABELS = {StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED,
                                             StrId::STR_STATS_THU, StrId::STR_STATS_FRI, StrId::STR_STATS_SAT,
                                             StrId::STR_STATS_SUN};
constexpr std::array<StrId, 4> TIME_LABELS = {StrId::STR_STATS_MORNING, StrId::STR_STATS_AFTERNOON,
                                              StrId::STR_STATS_EVENING, StrId::STR_STATS_NIGHT};
constexpr std::array<UIIcon, achievementCount()> ACHIEVEMENT_ICONS = {
    UIIcon::ReaderPage, UIIcon::Reading,          UIIcon::ReaderStats, UIIcon::Clock,
    UIIcon::Reading,    UIIcon::Recent,           UIIcon::Favorite,    UIIcon::Recent,
    UIIcon::Clock,      UIIcon::ReaderDictionary, UIIcon::Bookmark,    UIIcon::Files,
    UIIcon::Wifi,       UIIcon::Interface,        UIIcon::Transfer,    UIIcon::System,
};

std::string titleFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t end = dot == std::string::npos || dot < start ? path.size() : dot;
  return path.substr(start, end - start);
}

uint8_t loadProgressPercent(const std::string& cachePath) {
  HalFile file;
  if (!Storage.openFileForRead("RSTATS", cachePath + "/progress.bin", file)) return 0;
  uint8_t data[7]{};
  const int count = file.read(data, sizeof(data));
  file.close();
  return count == static_cast<int>(sizeof(data)) && data[6] <= 100 ? data[6] : 0;
}

void formatDuration(const uint32_t seconds, char* out, const size_t len) {
  if (seconds == 0) {
    snprintf(out, len, tr(STR_HOME_MINUTES_FORMAT), 0UL);
  } else if (seconds < 60) {
    snprintf(out, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
  } else {
    const uint32_t hours = seconds / 3600;
    const uint32_t minutes = (seconds % 3600) / 60;
    if (hours == 0) {
      snprintf(out, len, tr(STR_HOME_MINUTES_FORMAT), static_cast<unsigned long>(minutes));
    } else {
      snprintf(out, len, tr(STR_HOME_HOURS_MINUTES_FORMAT), static_cast<unsigned long>(hours),
               static_cast<unsigned long>(minutes));
    }
  }
}

int fittedFont(const GfxRenderer& renderer, const char* text, const int width, const bool large) {
  const int candidates[] = {large ? UI_14_FONT_ID : UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID};
  for (const int font : candidates) {
    if (renderer.getTextWidth(font, text, EpdFontFamily::BOLD) <= width) return font;
  }
  return SMALL_FONT_ID;
}

void drawCenteredInRect(const GfxRenderer& renderer, const int font, const Rect& rect, const int y, const char* text,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int textWidth = renderer.getTextWidth(font, text, style);
  renderer.drawText(font, rect.x + std::max(0, (rect.width - textWidth) / 2), y, text, true, style);
}

void drawMetricCard(const GfxRenderer& renderer, const Rect& rect, const char* value, const char* label,
                    const bool hero = false) {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, CARD_RADIUS, true);
  if (hero) renderer.fillRoundedRect(rect.x + 10, rect.y + 12, 5, rect.height - 24, 2, Color::Black);

  const int innerWidth = rect.width - (hero ? 44 : 20);
  const int font = fittedFont(renderer, value, innerWidth, hero);
  const int valueY = rect.y + (hero ? 18 : 14);
  drawCenteredInRect(renderer, font, rect, valueY, value, EpdFontFamily::BOLD);

  const auto lines = renderer.wrappedText(SMALL_FONT_ID, label, rect.width - 24, 2);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int blockHeight = static_cast<int>(lines.size()) * lineHeight;
  int labelY = rect.y + rect.height - blockHeight - 10;
  for (const auto& line : lines) {
    drawCenteredInRect(renderer, SMALL_FONT_ID, rect, labelY, line.c_str());
    labelY += lineHeight;
  }
}

void drawPageHeader(const GfxRenderer& renderer, const char* title) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

int pageContentTop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

void drawBackHints(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void drawVerticalBars(const GfxRenderer& renderer, const Rect& rect, const uint32_t* values, const char* const* labels,
                      const int count, const int labelFont = SMALL_FONT_ID) {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, CARD_RADIUS, true);
  uint32_t maxValue = 0;
  for (int i = 0; i < count; ++i) maxValue = std::max(maxValue, values[i]);

  const int slotW = (rect.width - 20) / count;
  const int labelH = renderer.getLineHeight(labelFont);
  const int chartBottom = rect.y + rect.height - labelH - 18;
  const int chartTop = rect.y + 22;
  const int maxBarH = std::max(1, chartBottom - chartTop - 8);
  renderer.drawLine(rect.x + 10, chartBottom, rect.x + rect.width - 11, chartBottom, true);

  if (maxValue == 0) {
    const char* empty = tr(STR_STATS_NO_ACTIVITY);
    drawCenteredInRect(renderer, SMALL_FONT_ID, rect,
                       chartTop + std::max(0, (chartBottom - chartTop - renderer.getLineHeight(SMALL_FONT_ID)) / 2),
                       empty);
  }

  for (int i = 0; i < count; ++i) {
    const int barW = std::clamp(slotW / 2, 10, 28);
    const int barH = values[i] > 0 && maxValue > 0
                         ? std::max(4, static_cast<int>((static_cast<uint64_t>(maxBarH) * values[i]) / maxValue))
                         : 0;
    const int slotX = rect.x + 10 + i * slotW;
    const int x = slotX + (slotW - barW) / 2;
    if (barH > 0) renderer.fillRoundedRect(x, chartBottom - barH, barW, barH, std::min(4, barW / 2), Color::Black);
    const int labelWidth = renderer.getTextWidth(labelFont, labels[i]);
    renderer.drawText(labelFont, slotX + (slotW - labelWidth) / 2, chartBottom + 8, labels[i]);
  }
}

template <size_t N>
uint32_t totalValues(const std::array<uint32_t, N>& values) {
  return std::accumulate(values.begin(), values.end(), uint32_t{0}, addReadingStatsSaturated);
}

template <size_t N>
void drawDistributionCard(const GfxRenderer& renderer, const Rect& rect, const char* title,
                          const std::array<uint32_t, N>& values, const std::array<StrId, N>& labels) {
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, CARD_RADIUS, true);
  drawCenteredInRect(renderer, UI_10_FONT_ID, rect, rect.y + 12, title, EpdFontFamily::BOLD);
  const int dividerY = rect.y + 48;
  renderer.drawLine(rect.x + 12, dividerY, rect.x + rect.width - 13, dividerY, true);

  const uint32_t total = totalValues(values);
  if (total == 0) {
    drawCenteredInRect(renderer, SMALL_FONT_ID, rect,
                       dividerY + (rect.y + rect.height - dividerY - renderer.getLineHeight(SMALL_FONT_ID)) / 2,
                       tr(STR_STATS_NO_ACTIVITY));
    return;
  }

  const int rowTop = dividerY + 8;
  const int rowH = (rect.height - 60) / static_cast<int>(N);
  const int labelW = 76;
  const int percentW = 42;
  const int barX = rect.x + 14 + labelW;
  const int barW = rect.width - 28 - labelW - percentW - 8;
  for (size_t i = 0; i < N; ++i) {
    const int y = rowTop + static_cast<int>(i) * rowH;
    const char* label = I18N.get(labels[i]);
    const std::string visible = renderer.truncatedText(SMALL_FONT_ID, label, labelW - 4);
    renderer.drawText(SMALL_FONT_ID, rect.x + 14, y + std::max(0, (rowH - renderer.getLineHeight(SMALL_FONT_ID)) / 2),
                      visible.c_str());
    const int barHeight = 10;
    const int barY = y + (rowH - barHeight) / 2;
    renderer.drawRoundedRect(barX, barY, barW, barHeight, 1, barHeight / 2, true);
    const int fill = static_cast<int>((static_cast<uint64_t>(barW) * values[i]) / total);
    if (fill > 0) renderer.fillRoundedRect(barX, barY, std::max(2, fill), barHeight, barHeight / 2, Color::Black);
    char percent[8];
    snprintf(percent, sizeof(percent), "%lu%%", static_cast<unsigned long>((values[i] * 100ULL) / total));
    renderer.drawText(MICRO_FONT_ID, barX + barW + 8,
                      y + std::max(0, (rowH - renderer.getLineHeight(MICRO_FONT_ID)) / 2), percent);
  }
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  stats = GlobalReadingStats::load();
  hasClock = getCurrentLocalReadingStatsDateTime(today);
  loadBooks();
  ACHIEVEMENTS.refresh(displayStats);
  requestUpdate();
}

void ReadingStatsActivity::loadBooks() {
  books.clear();
  books.reserve(RECENT_BOOKS.getBooks().size());
  ReadingStatsBookTotals bookTotals;
  for (const auto& recent : RECENT_BOOKS.getBooks()) {
    const std::string cachePath = getBookCachePath(recent.path);
    if (cachePath.empty()) continue;
    const BookReadingStats bookStats = BookReadingStats::load(cachePath);
    if (bookStats.totalReadingSeconds == 0 && bookStats.sessionCount == 0 && bookStats.totalPagesTurned == 0) continue;
    addBookToReadingStatsTotals(bookTotals, bookStats);
    books.push_back({recent.title.empty() ? titleFromPath(recent.path) : recent.title, cachePath, bookStats,
                     loadProgressPercent(cachePath)});
  }
  std::stable_sort(books.begin(), books.end(), [](const BookRow& left, const BookRow& right) {
    return left.stats.totalReadingSeconds > right.stats.totalReadingSeconds;
  });
  displayStats = stats;
  applyBookTotalsFloor(displayStats, bookTotals);
  selectedBook = std::min(selectedBook, books.empty() ? 0 : static_cast<int>(books.size()) - 1);
}

void ReadingStatsActivity::openMenuItem() {
  static constexpr std::array<Page, 6> PAGES = {Page::Overview, Page::Days,   Page::Weeks,
                                                Page::Books,    Page::Habits, Page::Achievements};
  if (selectedIndex >= 0 && selectedIndex < static_cast<int>(PAGES.size())) {
    page = PAGES[selectedIndex];
    requestUpdate();
  }
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (page == Page::Menu) {
      onGoHome(HomeMenuItem::READING_STATS);
    } else {
      page = Page::Menu;
      requestUpdate();
    }
    return;
  }

  if (page == Page::Menu) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedIndex = (selectedIndex + 1) % 6;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedIndex = (selectedIndex + 5) % 6;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openMenuItem();
    }
    return;
  }

  if (page == Page::Achievements) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedAchievement = (selectedAchievement + 1) % static_cast<int>(achievementCount());
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedAchievement =
          (selectedAchievement + static_cast<int>(achievementCount()) - 1) % static_cast<int>(achievementCount());
      requestUpdate();
    }
    return;
  }

  if (page == Page::Books && !books.empty()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedBook = (selectedBook + 1) % static_cast<int>(books.size());
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedBook = (selectedBook + static_cast<int>(books.size()) - 1) % static_cast<int>(books.size());
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const BookRow& book = books[selectedBook];
      startActivityForResult(makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, book.title, book.cachePath,
                                                                  book.stats, -1.0f, false, 0, displayStats, false),
                             [this](const ActivityResult&) {
                               stats = GlobalReadingStats::load();
                               loadBooks();
                             });
    }
  }
}

void ReadingStatsActivity::renderMenu() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_READING_STATS));
  constexpr std::array<UIIcon, 6> ICONS = {UIIcon::ReaderStats, UIIcon::Clock,   UIIcon::Recent,
                                           UIIcon::Book,        UIIcon::Reading, UIIcon::Favorite};
  const std::array<const char*, 6> labels = {tr(STR_STATS_OVERVIEW),       tr(STR_STATS_LAST_7_DAYS),
                                             tr(STR_STATS_LAST_8_WEEKS),   tr(STR_BOOKS),
                                             tr(STR_STATS_READING_HABITS), tr(STR_ACHIEVEMENTS)};
  std::array<std::string, 6> values;
  char duration[40];
  if (hasClock) {
    formatDuration(displayStats.readingSecondsForDay(today.date), duration, sizeof(duration));
    values[0] = duration;
    formatDuration(displayStats.readingSecondsForDaysEnding(today.date, 7), duration, sizeof(duration));
    values[1] = duration;
    formatDuration(displayStats.readingSecondsForDaysEnding(today.date, 56), duration, sizeof(duration));
    values[2] = duration;
  }
  values[3] = std::to_string(books.size());
  values[5] = std::to_string(ACHIEVEMENTS.unlockedCount(displayStats)) + "/" + std::to_string(achievementCount());
  const int top = pageContentTop();
  GUI.drawList(
      renderer, Rect{0, top, width, UITheme::getListContentBottom(renderer, false) - top}, labels.size(), selectedIndex,
      [&](const int i) { return std::string(labels[i]); }, nullptr, [&](const int i) { return ICONS[i]; },
      [&](const int i) { return values[i]; });
  const auto hints = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void ReadingStatsActivity::renderOverview() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_STATS_OVERVIEW));
  uint32_t todaySeconds = 0;
  uint32_t weekSeconds = 0;
  uint16_t streak = 0;
  if (hasClock) {
    todaySeconds = displayStats.readingSecondsForDay(today.date);
    const uint8_t weekday = readingStatsDayOfWeekIndex(today.date);
    weekSeconds = displayStats.readingSecondsForDaysEnding(today.date, static_cast<uint16_t>(weekday + 1));
    streak = displayStats.currentReadingStreak(&today.date);
  } else {
    streak = displayStats.currentReadingStreak(nullptr);
  }

  const int contentTop = pageContentTop();
  const int contentW = width - PAGE_MARGIN * 2;
  char value[48];
  formatDuration(todaySeconds, value, sizeof(value));
  drawMetricCard(renderer, Rect{PAGE_MARGIN, contentTop, contentW, 120}, value, tr(STR_STATS_TODAY), true);

  const int sectionY = contentTop + 134;
  renderer.drawText(UI_10_FONT_ID, PAGE_MARGIN, sectionY, tr(STR_STATS_ALL_TIME), true, EpdFontFamily::BOLD);
  renderer.drawLine(PAGE_MARGIN, sectionY + 34, width - PAGE_MARGIN - 1, sectionY + 34, true);

  const int gridTop = sectionY + 48;
  const int cellW = (contentW - CARD_GAP) / 2;
  const int cellH = 108;
  formatDuration(weekSeconds, value, sizeof(value));
  drawMetricCard(renderer, Rect{PAGE_MARGIN, gridTop, cellW, cellH}, value, tr(STR_STATS_THIS_WEEK));
  snprintf(value, sizeof(value), "%u %s", static_cast<unsigned>(streak),
           streak == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS));
  drawMetricCard(renderer, Rect{PAGE_MARGIN + cellW + CARD_GAP, gridTop, cellW, cellH}, value,
                 tr(STR_STATS_READING_STREAK_LBL));
  formatDuration(displayStats.totalReadingSeconds, value, sizeof(value));
  drawMetricCard(renderer, Rect{PAGE_MARGIN, gridTop + cellH + CARD_GAP, cellW, cellH}, value,
                 tr(STR_STATS_TOTAL_READING_TIME_LBL));
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(displayStats.completedBooks));
  drawMetricCard(renderer, Rect{PAGE_MARGIN + cellW + CARD_GAP, gridTop + cellH + CARD_GAP, cellW, cellH}, value,
                 tr(STR_STATS_COMPLETED_LBL));
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(displayStats.totalSessions));
  drawMetricCard(renderer, Rect{PAGE_MARGIN, gridTop + (cellH + CARD_GAP) * 2, contentW, 88}, value,
                 tr(STR_STATS_SESSIONS_LBL));
  drawBackHints(renderer, mappedInput);
}

void ReadingStatsActivity::renderDays() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_STATS_LAST_7_DAYS));
  std::array<uint32_t, 7> values{};
  std::array<const char*, 7> labels{};
  uint32_t total = 0;
  uint16_t active = 0;
  if (hasClock) {
    ReadingStatsDate date = today.date;
    addDaysToReadingStatsDate(date, -6);
    for (size_t i = 0; i < values.size(); ++i) {
      values[i] = displayStats.readingSecondsForDay(date);
      labels[i] = I18N.get(DAY_LABELS[readingStatsDayOfWeekIndex(date)]);
      total = addReadingStatsSaturated(total, values[i]);
      if (values[i] > 0) ++active;
      addDaysToReadingStatsDate(date, 1);
    }
  } else {
    for (size_t i = 0; i < labels.size(); ++i) labels[i] = I18N.get(DAY_LABELS[i]);
  }

  const int top = pageContentTop();
  const int contentW = width - PAGE_MARGIN * 2;
  const int cellW = (contentW - CARD_GAP) / 2;
  char duration[40];
  char activeText[16];
  formatDuration(total, duration, sizeof(duration));
  snprintf(activeText, sizeof(activeText), "%u / 7", static_cast<unsigned>(active));
  drawMetricCard(renderer, Rect{PAGE_MARGIN, top, cellW, 100}, duration, tr(STR_STATS_TOTAL_READING_TIME_LBL));
  drawMetricCard(renderer, Rect{PAGE_MARGIN + cellW + CARD_GAP, top, cellW, 100}, activeText,
                 tr(STR_STATS_ACTIVE_DAYS));
  drawVerticalBars(renderer, Rect{PAGE_MARGIN, top + 112, contentW, 430}, values.data(), labels.data(), 7);
  drawBackHints(renderer, mappedInput);
}

void ReadingStatsActivity::renderWeeks() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_STATS_LAST_8_WEEKS));
  std::array<uint32_t, 8> values{};
  std::array<std::array<char, 8>, 8> labelStorage{};
  std::array<const char*, 8> labels{};
  uint32_t total = 0;
  uint32_t best = 0;
  if (hasClock) {
    const uint8_t weekday = readingStatsDayOfWeekIndex(today.date);
    ReadingStatsDate currentWeekStart = today.date;
    addDaysToReadingStatsDate(currentWeekStart, -weekday);
    for (int i = 0; i < 8; ++i) {
      ReadingStatsDate weekStart = currentWeekStart;
      addDaysToReadingStatsDate(weekStart, -(7 - i) * 7);
      ReadingStatsDate weekEnd = weekStart;
      addDaysToReadingStatsDate(weekEnd, 6);
      values[i] = displayStats.readingSecondsForDaysEnding(weekEnd, 7);
      total = addReadingStatsSaturated(total, values[i]);
      best = std::max(best, values[i]);
      if (i == 0 || i == 2 || i == 4 || i == 7) {
        snprintf(labelStorage[i].data(), labelStorage[i].size(), "%u/%u", static_cast<unsigned>(weekStart.day),
                 static_cast<unsigned>(weekStart.month));
      }
      labels[i] = labelStorage[i].data();
    }
  } else {
    for (int i = 0; i < 8; ++i) {
      if (i == 0 || i == 2 || i == 4 || i == 7) snprintf(labelStorage[i].data(), labelStorage[i].size(), "%d", i + 1);
      labels[i] = labelStorage[i].data();
    }
  }

  const int top = pageContentTop();
  const int contentW = width - PAGE_MARGIN * 2;
  const int cellW = (contentW - CARD_GAP) / 2;
  char totalText[40];
  char bestText[40];
  formatDuration(total, totalText, sizeof(totalText));
  formatDuration(best, bestText, sizeof(bestText));
  drawMetricCard(renderer, Rect{PAGE_MARGIN, top, cellW, 100}, totalText, tr(STR_STATS_TOTAL_READING_TIME_LBL));
  drawMetricCard(renderer, Rect{PAGE_MARGIN + cellW + CARD_GAP, top, cellW, 100}, bestText, tr(STR_STATS_BEST_WEEK));
  drawVerticalBars(renderer, Rect{PAGE_MARGIN, top + 112, contentW, 430}, values.data(), labels.data(), 8,
                   MICRO_FONT_ID);
  drawBackHints(renderer, mappedInput);
}

void ReadingStatsActivity::renderBooks() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_BOOKS));
  const int top = pageContentTop();
  const int height = UITheme::getListContentBottom(renderer, !books.empty()) - top;
  if (books.empty()) {
    GUI.drawEmptyState(renderer, Rect{0, top, width, height}, tr(STR_STATS_NO_BOOK_DATA), nullptr, true);
  } else {
    GUI.drawList(
        renderer, Rect{0, top, width, height}, books.size(), selectedBook, [&](const int i) { return books[i].title; },
        [&](const int i) {
          char duration[40];
          formatDuration(books[i].stats.totalReadingSeconds, duration, sizeof(duration));
          if (books[i].progressPercent == 0) return std::string(duration);
          return std::string(duration) + " · " + std::to_string(books[i].progressPercent) + "%";
        },
        [](const int) { return UIIcon::Book; });
    GUI.drawFooterCounter(renderer, selectedBook, books.size());
  }
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), books.empty() ? "" : tr(STR_OPEN), "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void ReadingStatsActivity::renderHabits() {
  renderer.clearScreen();
  drawPageHeader(renderer, tr(STR_STATS_READING_HABITS));
  const int top = pageContentTop();
  const int width = renderer.getScreenWidth();
  const int contentW = width - PAGE_MARGIN * 2;
  drawDistributionCard(renderer, Rect{PAGE_MARGIN, top, contentW, 250}, tr(STR_STATS_TIME_OF_DAY),
                       displayStats.timeOfDaySeconds, TIME_LABELS);
  drawDistributionCard(renderer, Rect{PAGE_MARGIN, top + 262, contentW, 286}, tr(STR_STATS_DAY_OF_WEEK),
                       displayStats.dayOfWeekSeconds, DAY_LABELS);
  drawBackHints(renderer, mappedInput);
}

void ReadingStatsActivity::renderAchievements() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_ACHIEVEMENTS));

  const int unlocked = ACHIEVEMENTS.unlockedCount(displayStats);
  const int top = pageContentTop();
  const Rect summary{PAGE_MARGIN, top, width - PAGE_MARGIN * 2, 82};
  renderer.drawRoundedRect(summary.x, summary.y, summary.width, summary.height, 1, CARD_RADIUS, true);
  renderer.fillRoundedRect(summary.x + 10, summary.y + 12, 5, summary.height - 24, 2, Color::Black);

  char countText[20];
  snprintf(countText, sizeof(countText), "%d / %u", unlocked, static_cast<unsigned>(achievementCount()));
  renderer.drawText(UI_12_FONT_ID, summary.x + 30, summary.y + 11, countText, true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, summary.x + 30, summary.y + 48, tr(STR_ACHIEVEMENTS_UNLOCKED));

  const int barWidth = std::min(180, summary.width / 2);
  const int barX = summary.x + summary.width - barWidth - 18;
  const int barY = summary.y + (summary.height - 10) / 2;
  renderer.fillRoundedRect(barX, barY, barWidth, 10, 5, Color::LightGray);
  const int fill = barWidth * unlocked / static_cast<int>(achievementCount());
  if (fill > 0) renderer.fillRoundedRect(barX, barY, std::max(3, fill), 10, 5, Color::Black);

  const int listTop = summary.y + summary.height + 10;
  const int listBottom = UITheme::getListContentBottom(renderer, true);
  GUI.drawList(
      renderer, Rect{0, listTop, width, std::max(0, listBottom - listTop)}, static_cast<int>(achievementCount()),
      selectedAchievement,
      [&](const int index) { return std::string(ACHIEVEMENTS.name(static_cast<AchievementId>(index))); },
      [&](const int index) { return std::string(ACHIEVEMENTS.description(static_cast<AchievementId>(index))); },
      [](const int index) { return ACHIEVEMENT_ICONS[index]; },
      [&](const int index) {
        const AchievementView item = ACHIEVEMENTS.view(static_cast<AchievementId>(index), displayStats);
        if (item.unlocked) return std::string();
        const uint32_t percent = item.target == 0 ? 0 : (static_cast<uint64_t>(item.current) * 100u) / item.target;
        return std::to_string(percent) + "%";
      },
      false,
      [&](const int index) { return !ACHIEVEMENTS.view(static_cast<AchievementId>(index), displayStats).unlocked; },
      [&](const int index) {
        return ACHIEVEMENTS.view(static_cast<AchievementId>(index), displayStats).unlocked ? UIAccessory::Check
                                                                                           : UIAccessory::None;
      });
  GUI.drawFooterCounter(renderer, selectedAchievement, achievementCount());
  drawBackHints(renderer, mappedInput);
}

void ReadingStatsActivity::render(RenderLock&&) {
  switch (page) {
    case Page::Menu:
      renderMenu();
      break;
    case Page::Overview:
      renderOverview();
      break;
    case Page::Days:
      renderDays();
      break;
    case Page::Weeks:
      renderWeeks();
      break;
    case Page::Books:
      renderBooks();
      break;
    case Page::Habits:
      renderHabits();
      break;
    case Page::Achievements:
      renderAchievements();
      break;
  }
  renderer.displayBuffer();
}
