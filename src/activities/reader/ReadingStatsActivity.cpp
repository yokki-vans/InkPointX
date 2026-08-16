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
#include "achievements/AchievementVisuals.h"
#include "activities/util/IntervalSelectionActivity.h"
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

int drawCenteredWrapped(const GfxRenderer& renderer, const int font, const char* text, const Rect& rect, const int y,
                        const int maxLines, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const auto lines = renderer.wrappedText(font, text, rect.width, maxLines, style);
  int lineY = y;
  const int lineHeight = renderer.getLineHeight(font);
  for (const auto& line : lines) {
    drawCenteredInRect(renderer, font, rect, lineY, line.c_str(), style);
    lineY += lineHeight;
  }
  return lineY;
}

std::string achievementProgressValue(const AchievementMetric metric, const uint32_t value) {
  if (metric == AchievementMetric::ReadingSeconds || metric == AchievementMetric::NightSeconds) {
    char duration[40];
    formatDuration(value, duration, sizeof(duration));
    return duration;
  }
  return std::to_string(value);
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
  static constexpr std::array<Page, 7> PAGES = {Page::Overview, Page::Days, Page::Weeks,       Page::Books,
                                                Page::Habits,   Page::Goal, Page::Achievements};
  if (selectedIndex >= 0 && selectedIndex < static_cast<int>(PAGES.size())) {
    page = PAGES[selectedIndex];
    requestUpdate();
  }
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (page == Page::Menu) {
      onGoHome(HomeMenuItem::READING_STATS);
    } else if (page == Page::AchievementDetail) {
      page = Page::Achievements;
      requestUpdate();
    } else {
      page = Page::Menu;
      requestUpdate();
    }
    return;
  }

  if (page == Page::Menu) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedIndex = (selectedIndex + 1) % 7;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedIndex = (selectedIndex + 6) % 7;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openMenuItem();
    }
    return;
  }

  if (page == Page::Goal) {
    const bool next = mappedInput.wasPressed(MappedInputManager::Button::Down);
    const bool previous = mappedInput.wasPressed(MappedInputManager::Button::Up);
    if (next || previous) {
      selectedGoalSetting = (selectedGoalSetting + (next ? 1 : 2)) % 3;
      requestUpdate();
      return;
    }

    const bool left = mappedInput.wasPressed(MappedInputManager::Button::Left);
    const bool right = mappedInput.wasPressed(MappedInputManager::Button::Right);
    const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (!left && !right && !confirm) return;

    ReadingGoalSettings settings = READING_GOAL.settings();
    if (selectedGoalSetting == 0) {
      READING_GOAL.setEnabled(!settings.enabled);
      requestUpdate();
    } else if (selectedGoalSetting == 1 && (left || right)) {
      const int adjusted = std::clamp(static_cast<int>(settings.dailyMinutes) + (right ? 5 : -5), 5, 240);
      READING_GOAL.setDailyMinutes(static_cast<uint16_t>(adjusted));
      requestUpdate();
    } else if (selectedGoalSetting == 1 && confirm) {
      startActivityForResult(
          makeUniqueNoThrow<IntervalSelectionActivity>(renderer, mappedInput, "ReadingGoalInterval",
                                                       StrId::STR_READING_GOAL_DAILY_TARGET,
                                                       StrId::STR_READING_GOAL_STEP_HINT, settings.dailyMinutes, 5, 240,
                                                       5, 30, StrId::STR_READING_GOAL_MINUTES_FORMAT, false, true),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              READING_GOAL.setDailyMinutes(static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
            }
            stats = GlobalReadingStats::load();
            loadBooks();
            requestUpdate();
          });
    } else if (selectedGoalSetting == 2) {
      READING_GOAL.setRemindersEnabled(!settings.remindersEnabled);
      requestUpdate();
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
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      page = Page::AchievementDetail;
      requestUpdate();
    }
    return;
  }

  if (page == Page::AchievementDetail) {
    const bool next = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                      mappedInput.wasPressed(MappedInputManager::Button::Right);
    const bool previous = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (next || previous) {
      selectedAchievement = (selectedAchievement + (next ? 1 : static_cast<int>(achievementCount()) - 1)) %
                            static_cast<int>(achievementCount());
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
  constexpr std::array<UIIcon, 7> ICONS = {UIIcon::ReaderStats, UIIcon::Clock,    UIIcon::Recent,  UIIcon::Book,
                                           UIIcon::Reading,     UIIcon::Favorite, UIIcon::Favorite};
  const std::array<const char*, 7> labels = {
      tr(STR_STATS_OVERVIEW),       tr(STR_STATS_LAST_7_DAYS), tr(STR_STATS_LAST_8_WEEKS), tr(STR_BOOKS),
      tr(STR_STATS_READING_HABITS), tr(STR_READING_GOAL),      tr(STR_ACHIEVEMENTS)};
  std::array<std::string, 7> values;
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
  const ReadingGoalSettings goal = READING_GOAL.settings();
  if (goal.enabled && hasClock) {
    const ReadingGoalProgress progress = READING_GOAL.progress(displayStats, &today);
    char goalProgress[32];
    snprintf(goalProgress, sizeof(goalProgress), tr(STR_READING_GOAL_PROGRESS_FORMAT),
             static_cast<unsigned>(std::min<uint32_t>(progress.todaySeconds / 60u, goal.dailyMinutes)),
             static_cast<unsigned>(goal.dailyMinutes));
    values[5] = goalProgress;
  } else {
    values[5] = tr(STR_DISABLED);
  }
  values[6] = std::to_string(ACHIEVEMENTS.unlockedCount(displayStats)) + "/" + std::to_string(achievementCount());
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

void ReadingStatsActivity::renderGoal() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_READING_GOAL));
  const int top = pageContentTop();
  const int contentW = width - PAGE_MARGIN * 2;
  const ReadingGoalSettings settings = READING_GOAL.settings();
  const ReadingGoalProgress goal = hasClock ? READING_GOAL.progress(displayStats, &today) : ReadingGoalProgress{};

  const Rect summary{PAGE_MARGIN, top, contentW, 126};
  renderer.drawRoundedRect(summary.x, summary.y, summary.width, summary.height, 1, CARD_RADIUS, true);
  renderer.fillRoundedRect(summary.x + 10, summary.y + 12, 5, summary.height - 24, 2, Color::Black);

  char progressText[40];
  if (!settings.enabled) {
    snprintf(progressText, sizeof(progressText), "%s", tr(STR_DISABLED));
  } else if (!hasClock) {
    snprintf(progressText, sizeof(progressText), "%s", tr(STR_READING_GOAL_CLOCK_REQUIRED));
  } else {
    snprintf(progressText, sizeof(progressText), tr(STR_READING_GOAL_PROGRESS_FORMAT),
             static_cast<unsigned>(goal.todaySeconds / 60u), static_cast<unsigned>(settings.dailyMinutes));
  }
  renderer.drawText(UI_12_FONT_ID, summary.x + 30, summary.y + 14, progressText, true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, summary.x + 30, summary.y + 52, tr(STR_READING_GOAL_TODAY));

  const int barX = summary.x + 30;
  const int barY = summary.y + 88;
  const int barW = summary.width - 60;
  renderer.fillRoundedRect(barX, barY, barW, 12, 6, Color::LightGray);
  if (settings.enabled && goal.targetSeconds > 0) {
    const int fill = static_cast<int>(
        std::min<uint64_t>(barW, static_cast<uint64_t>(barW) * goal.todaySeconds / goal.targetSeconds));
    if (fill > 0) renderer.fillRoundedRect(barX, barY, std::max(3, fill), 12, 6, Color::Black);
  }

  const int cardsY = top + 138;
  const int cellW = (contentW - CARD_GAP) / 2;
  char streakText[20];
  char weekText[20];
  snprintf(streakText, sizeof(streakText), "%u %s", static_cast<unsigned>(goal.currentStreak),
           goal.currentStreak == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS));
  snprintf(weekText, sizeof(weekText), "%u / 7", static_cast<unsigned>(goal.completedLast7Days));
  drawMetricCard(renderer, Rect{PAGE_MARGIN, cardsY, cellW, 96}, streakText, tr(STR_READING_GOAL_STREAK));
  drawMetricCard(renderer, Rect{PAGE_MARGIN + cellW + CARD_GAP, cardsY, cellW, 96}, weekText,
                 tr(STR_READING_GOAL_LAST_7));

  const std::array<const char*, 3> labels = {tr(STR_READING_GOAL_TRACKER), tr(STR_READING_GOAL_DAILY_TARGET),
                                             tr(STR_READING_GOAL_REMINDERS)};
  std::array<std::string, 3> values;
  values[0] = settings.enabled ? tr(STR_ENABLED) : tr(STR_DISABLED);
  char targetText[24];
  snprintf(targetText, sizeof(targetText), tr(STR_READING_GOAL_MINUTES_FORMAT),
           static_cast<unsigned>(settings.dailyMinutes));
  values[1] = targetText;
  values[2] = settings.remindersEnabled ? tr(STR_ENABLED) : tr(STR_DISABLED);
  constexpr std::array<UIIcon, 3> icons = {UIIcon::Favorite, UIIcon::Clock, UIIcon::System};
  const int listTop = cardsY + 108;
  const int listBottom = UITheme::getListContentBottom(renderer, false);
  GUI.drawList(
      renderer, Rect{0, listTop, width, std::max(0, listBottom - listTop)}, labels.size(), selectedGoalSetting,
      [&](const int i) { return std::string(labels[i]); }, nullptr, [&](const int i) { return icons[i]; },
      [&](const int i) { return values[i]; });
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DECREASE), tr(STR_INCREASE));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void ReadingStatsActivity::renderAchievements() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_ACHIEVEMENTS));

  const int unlocked = ACHIEVEMENTS.unlockedCount(displayStats);
  const int top = pageContentTop();
  const Rect summary{PAGE_MARGIN, top, width - PAGE_MARGIN * 2, 96};
  renderer.drawRoundedRect(summary.x, summary.y, summary.width, summary.height, 1, CARD_RADIUS, true);
  drawAchievementMedallion(renderer, AchievementId::FirstPage, summary.x + 14, summary.y + 14, 58, true);

  char countText[20];
  snprintf(countText, sizeof(countText), "%d / %u", unlocked, static_cast<unsigned>(achievementCount()));
  renderer.drawText(UI_12_FONT_ID, summary.x + 88, summary.y + 11, countText, true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, summary.x + 88, summary.y + 48, tr(STR_ACHIEVEMENTS_UNLOCKED));

  const int barX = summary.x + 88;
  const int barWidth = summary.x + summary.width - 16 - barX;
  const int barY = summary.y + summary.height - 22;
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
      [](const int index) { return achievementIcon(static_cast<AchievementId>(index)); },
      [&](const int index) {
        const AchievementView item = ACHIEVEMENTS.view(static_cast<AchievementId>(index), displayStats);
        if (item.unlocked) return std::string(tr(STR_ACHIEVEMENTS_UNLOCKED));
        const uint32_t percent = item.target == 0 ? 0 : (static_cast<uint64_t>(item.current) * 100u) / item.target;
        return std::to_string(percent) + "%";
      },
      false, [&](const int index) { return !ACHIEVEMENTS.isUnlocked(static_cast<AchievementId>(index)); },
      [](const int) { return UIAccessory::Chevron; });
  GUI.drawFooterCounter(renderer, selectedAchievement, achievementCount());
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void ReadingStatsActivity::renderAchievementDetail() {
  renderer.clearScreen();
  const int width = renderer.getScreenWidth();
  drawPageHeader(renderer, tr(STR_ACHIEVEMENTS));

  const AchievementId id = static_cast<AchievementId>(selectedAchievement);
  const AchievementView item = ACHIEVEMENTS.view(id, displayStats);
  const AchievementMetric metric = ACHIEVEMENT_DEFINITIONS[selectedAchievement].metric;
  const int top = pageContentTop();
  const int contentWidth = width - PAGE_MARGIN * 2;
  const Rect textLane{PAGE_MARGIN + 12, 0, contentWidth - 24, 0};

  constexpr int medallionSize = 92;
  drawAchievementMedallion(renderer, id, (width - medallionSize) / 2, top + 8, medallionSize, item.unlocked);

  const char* status = item.unlocked ? tr(STR_ACHIEVEMENTS_UNLOCKED) : tr(STR_ACH_LOCKED);
  const int statusW = renderer.getTextWidth(SMALL_FONT_ID, status, EpdFontFamily::BOLD) + 28;
  const int statusH = 28;
  const int statusX = (width - statusW) / 2;
  const int statusY = top + 112;
  if (item.unlocked) {
    renderer.fillRoundedRect(statusX, statusY, statusW, statusH, statusH / 2, Color::Black);
    renderer.drawText(SMALL_FONT_ID,
                      statusX + (statusW - renderer.getTextWidth(SMALL_FONT_ID, status, EpdFontFamily::BOLD)) / 2,
                      statusY + 5, status, false, EpdFontFamily::BOLD);
  } else {
    renderer.drawRoundedRect(statusX, statusY, statusW, statusH, 1, statusH / 2, true);
    renderer.drawText(SMALL_FONT_ID,
                      statusX + (statusW - renderer.getTextWidth(SMALL_FONT_ID, status, EpdFontFamily::BOLD)) / 2,
                      statusY + 5, status, true, EpdFontFamily::BOLD);
  }

  int textY = statusY + statusH + 18;
  textY = drawCenteredWrapped(renderer, UI_14_FONT_ID, item.name.c_str(), textLane, textY, 2, EpdFontFamily::BOLD) + 8;
  textY = drawCenteredWrapped(renderer, UI_10_FONT_ID, item.description.c_str(), textLane, textY, 3) + 18;

  const Rect progressCard{PAGE_MARGIN, textY, contentWidth, 112};
  renderer.drawRoundedRect(progressCard.x, progressCard.y, progressCard.width, progressCard.height, 1, CARD_RADIUS,
                           true);
  renderer.drawText(UI_10_FONT_ID, progressCard.x + 16, progressCard.y + 13, tr(STR_PROGRESS), true,
                    EpdFontFamily::BOLD);
  const std::string current = achievementProgressValue(metric, item.current);
  const std::string target = achievementProgressValue(metric, item.target);
  const std::string progressText = current + " / " + target;
  const int progressTextW = renderer.getTextWidth(SMALL_FONT_ID, progressText.c_str());
  renderer.drawText(SMALL_FONT_ID, progressCard.x + progressCard.width - progressTextW - 16, progressCard.y + 16,
                    progressText.c_str());
  const int barX = progressCard.x + 16;
  const int barY = progressCard.y + 58;
  const int barW = progressCard.width - 32;
  renderer.fillRoundedRect(barX, barY, barW, 14, 7, Color::LightGray);
  const int fill =
      item.target == 0
          ? 0
          : static_cast<int>(std::min<uint64_t>(barW, static_cast<uint64_t>(barW) * item.current / item.target));
  if (fill > 0) renderer.fillRoundedRect(barX, barY, std::max(4, fill), 14, 7, Color::Black);
  const uint32_t percent = item.target == 0 ? 0 : (static_cast<uint64_t>(item.current) * 100u) / item.target;
  char percentText[12];
  snprintf(percentText, sizeof(percentText), "%lu%%", static_cast<unsigned long>(percent));
  drawCenteredInRect(renderer, SMALL_FONT_ID, progressCard, progressCard.y + 82, percentText, EpdFontFamily::BOLD);

  if (item.unlocked) {
    const Rect earnedCard{PAGE_MARGIN, progressCard.y + progressCard.height + 12, contentWidth, 64};
    renderer.fillRoundedRect(earnedCard.x, earnedCard.y, earnedCard.width, earnedCard.height, CARD_RADIUS,
                             Color::LightGray);
    std::string earnedText;
    ReadingStatsDate earnedDate;
    if (item.earnedDayIndex != ACHIEVEMENT_DAY_UNKNOWN &&
        readingStatsDateFromDayIndex(item.earnedDayIndex, earnedDate)) {
      char date[16];
      snprintf(date, sizeof(date), "%04u-%02u-%02u", static_cast<unsigned>(earnedDate.year),
               static_cast<unsigned>(earnedDate.month), static_cast<unsigned>(earnedDate.day));
      char earned[64];
      snprintf(earned, sizeof(earned), tr(STR_ACH_EARNED_ON), date);
      earnedText = earned;
    } else {
      earnedText = tr(STR_ACH_EARNED_DATE_UNKNOWN);
    }
    drawCenteredWrapped(renderer, SMALL_FONT_ID, earnedText.c_str(),
                        Rect{earnedCard.x + 12, 0, earnedCard.width - 24, 0}, earnedCard.y + 19, 2,
                        EpdFontFamily::BOLD);
  }

  GUI.drawFooterCounter(renderer, selectedAchievement, achievementCount());
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
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
    case Page::Goal:
      renderGoal();
      break;
    case Page::Achievements:
      renderAchievements();
      break;
    case Page::AchievementDetail:
      renderAchievementDetail();
      break;
  }
  renderer.displayBuffer();
}
