#include "EndOfBookView.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <utility>

#include "FavoriteBooksStore.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/home/LibraryActivity.h"
#include "components/UITheme.h"
#include "components/icons/lucide_ui.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/ButtonNavigator.h"

namespace {
std::string titleFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t end = dot == std::string::npos || dot < start ? path.size() : dot;
  return path.substr(start, end - start);
}

void drawCentered(const GfxRenderer& renderer, const int font, const int x, const int width, const int y,
                  const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int textWidth = renderer.getTextWidth(font, text, style);
  renderer.drawText(font, x + std::max(0, (width - textWidth) / 2), y, text, true, style);
}

int drawCenteredWrapped(const GfxRenderer& renderer, const int font, const int x, const int width, const int y,
                        const char* text, const int maxLines,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const auto lines = renderer.wrappedText(font, text, width, maxLines, style);
  int lineY = y;
  for (const auto& line : lines) {
    drawCentered(renderer, font, x, width, lineY, line.c_str(), style);
    lineY += renderer.getLineHeight(font);
  }
  return lineY;
}

void drawCompletionMedallion(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int radius = std::max(12, size / 4);
  renderer.fillRoundedRect(x, y, size, size, radius, Color::White);
  renderer.drawRoundedRect(x, y, size, size, 2, radius, true);
  renderer.drawRoundedRect(x + 6, y + 6, size - 12, size - 12, 1, std::max(8, radius - 5), true);
  renderer.drawIcon(LucideCheck24, x + (size - 24) / 2, y + (size - 24) / 2, 24, 24);
}

void drawMetric(const GfxRenderer& renderer, const Rect& rect, const char* value, const char* label) {
  const std::string visibleValue = renderer.truncatedText(UI_12_FONT_ID, value, rect.width - 12, EpdFontFamily::BOLD);
  const bool compact = rect.height <= 54;
  drawCentered(renderer, UI_12_FONT_ID, rect.x, rect.width, rect.y + (compact ? 4 : 8), visibleValue.c_str(),
               EpdFontFamily::BOLD);
  const std::string visibleLabel = renderer.truncatedText(SMALL_FONT_ID, label, rect.width - 12);
  drawCentered(renderer, SMALL_FONT_ID, rect.x, rect.width, rect.y + (compact ? 29 : 38), visibleLabel.c_str());
}
}  // namespace

void EndOfBookView::addCandidate(const RecentBook& source, const std::string& currentPath) {
  if (recommendations.size() >= MAX_RECOMMENDATIONS || source.path.empty() || source.path == currentPath ||
      !Storage.exists(source.path.c_str())) {
    return;
  }
  if (std::any_of(recommendations.begin(), recommendations.end(),
                  [&](const RecentBook& item) { return item.path == source.path; })) {
    return;
  }

  const std::string cachePath = getBookCachePath(source.path);
  if (!cachePath.empty() && BookReadingStats::load(cachePath).isCompleted) return;

  RecentBook candidate = source;
  if (candidate.title.empty()) candidate.title = titleFromPath(candidate.path);
  recommendations.push_back(std::move(candidate));
}

void EndOfBookView::prepare(const std::string& currentPath, const std::string& title, const std::string& author,
                            const BookReadingStats& stats) {
  if (prepared) return;
  prepared = true;
  finishedTitle = title.empty() ? titleFromPath(currentPath) : title;
  finishedAuthor = author;
  readingSeconds = stats.totalReadingSeconds;
  selectedIndex = 0;
  recommendations.clear();
  recommendations.reserve(MAX_RECOMMENDATIONS);

  // A recently touched favourite is the strongest local signal available on
  // an offline reader.  Then use the remaining favourites and recent books.
  const auto& recent = RECENT_BOOKS.getBooks();
  const auto& favourites = FAVORITE_BOOKS.getBooks();
  for (const auto& book : recent) {
    if (FAVORITE_BOOKS.contains(book.path)) addCandidate(book, currentPath);
  }
  for (const auto& book : favourites) addCandidate(book, currentPath);
  for (const auto& book : recent) addCandidate(book, currentPath);

  // A new reader may only have the just-finished book in Recents. Fill the
  // remaining slots from the cached library catalogue so the celebration can
  // still offer genuinely unread books without rescanning the SD card.
  if (recommendations.size() < MAX_RECOMMENDATIONS) {
    std::vector<RecentBook> libraryCandidates;
    libraryCandidates.reserve(MAX_RECOMMENDATIONS * 8);
    LibraryActivity::appendRecommendationCandidates(libraryCandidates, currentPath, MAX_RECOMMENDATIONS * 8);
    for (const auto& book : libraryCandidates) addCandidate(book, currentPath);
  }
}

EndOfBookView::Action EndOfBookView::handleInput(MappedInputManager& mappedInput) {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    return Action::FileBrowser;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) return Action::Home;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    return recommendations.empty() ? Action::OpenLibrary : Action::OpenRecommendation;
  }
  if (recommendations.empty()) return Action::None;
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, recommendations.size());
    return Action::SelectionChanged;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, recommendations.size());
    return Action::SelectionChanged;
  }
  return Action::None;
}

const std::string& EndOfBookView::selectedPath() const {
  static const std::string empty;
  return recommendations.empty() ? empty : recommendations[selectedIndex].path;
}

void EndOfBookView::render(GfxRenderer& renderer, MappedInputManager& mappedInput) const {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const bool compact = height < 650;
  const int margin = std::clamp(width / 18, 18, 28);
  const int innerWidth = width - margin * 2;
  const int hintReserve = SETTINGS.showButtonHints ? UITheme::getInstance().getMetrics().buttonHintsHeight + 12 : 12;
  const int contentBottom = height - hintReserve;

  renderer.clearScreen();

  const int titleY = compact ? 8 : 18;
  drawCentered(renderer, SCRIPT_FONT_ID, margin, innerWidth, titleY, tr(STR_END_OF_BOOK));

  const int medallionSize = compact ? 52 : 66;
  const int medallionY = compact ? 52 : 66;
  drawCompletionMedallion(renderer, (width - medallionSize) / 2, medallionY, medallionSize);

  const int bookTitleY = medallionY + medallionSize + (compact ? 8 : 13);
  const int titleFont = compact ? UI_10_FONT_ID : UI_12_FONT_ID;
  int afterTitle = drawCenteredWrapped(renderer, titleFont, margin + 10, innerWidth - 20, bookTitleY,
                                       finishedTitle.c_str(), compact ? 1 : 2, EpdFontFamily::BOLD);
  if (!finishedAuthor.empty() && !compact) {
    const std::string author = renderer.truncatedText(SMALL_FONT_ID, finishedAuthor.c_str(), innerWidth - 36);
    drawCentered(renderer, SMALL_FONT_ID, margin + 18, innerWidth - 36, afterTitle + 2, author.c_str());
    afterTitle += renderer.getLineHeight(SMALL_FONT_ID) + 2;
  }

  char duration[32];
  BookReadingStats::formatDuration(readingSeconds, duration, sizeof(duration));
  const int statsY = std::max(afterTitle + (compact ? 5 : 8), compact ? 158 : 232);
  const int statsHeight = compact ? 52 : 64;
  const Rect statsCard{margin, statsY, innerWidth, statsHeight};
  renderer.drawRoundedRect(statsCard.x, statsCard.y, statsCard.width, statsCard.height, 1, 14, true);
  renderer.drawLine(width / 2, statsCard.y + 9, width / 2, statsCard.y + statsCard.height - 10, true);
  drawMetric(renderer, Rect{statsCard.x, statsCard.y, statsCard.width / 2, statsCard.height}, "100%", tr(STR_DONE));
  drawMetric(
      renderer,
      Rect{statsCard.x + statsCard.width / 2, statsCard.y, statsCard.width - statsCard.width / 2, statsCard.height},
      duration, tr(STR_READING_TIME));

  const int sectionY = statsY + statsHeight + (compact ? 8 : 15);
  drawCentered(renderer, UI_10_FONT_ID, margin, innerWidth, sectionY, tr(STR_NEXT_FIELD), EpdFontFamily::BOLD);
  const int rowsTop = sectionY + renderer.getLineHeight(UI_10_FONT_ID) + (compact ? 3 : 8);

  if (recommendations.empty()) {
    const Rect empty{margin, rowsTop, innerWidth, std::max(52, contentBottom - rowsTop - 8)};
    renderer.drawRoundedRect(empty.x, empty.y, empty.width, empty.height, 1, 14, true);
    drawCenteredWrapped(renderer, UI_10_FONT_ID, empty.x + 18, empty.width - 36,
                        empty.y + std::max(8, (empty.height - renderer.getLineHeight(UI_10_FONT_ID) * 2) / 2),
                        tr(STR_OPEN_LIBRARY_HINT), 2);
  } else {
    const int gap = compact ? 5 : 8;
    const int available = std::max(1, contentBottom - rowsTop - gap * (static_cast<int>(recommendations.size()) - 1));
    const int rowHeight =
        std::clamp(available / static_cast<int>(recommendations.size()), compact ? 46 : 62, compact ? 58 : 78);
    const int blockHeight =
        rowHeight * static_cast<int>(recommendations.size()) + gap * (static_cast<int>(recommendations.size()) - 1);
    const int firstRowY = rowsTop + std::max(0, (contentBottom - rowsTop - blockHeight) / 2);
    for (size_t i = 0; i < recommendations.size(); ++i) {
      const bool selected = i == selectedIndex;
      const int y = firstRowY + static_cast<int>(i) * (rowHeight + gap);
      const Rect row{margin, y, innerWidth, rowHeight};
      renderer.fillRoundedRect(row.x, row.y, row.width, row.height, 12, Color::White);
      renderer.drawRoundedRect(row.x, row.y, row.width, row.height, selected ? 2 : 1, 12, true);
      if (selected) renderer.fillRoundedRect(row.x + 8, row.y + 10, 4, row.height - 20, 2, Color::Black);

      const int iconSize = 32;
      const int iconX = row.x + 20;
      const int iconY = row.y + (row.height - iconSize) / 2;
      renderer.drawIcon(LucideBookOpen32, iconX, iconY, iconSize, iconSize);
      const int chevronX = row.x + row.width - 34;
      renderer.drawIcon(LucideChevronRight24, chevronX, row.y + (row.height - 24) / 2, 24, 24);

      const int textX = iconX + iconSize + 12;
      const int textWidth = chevronX - textX - 8;
      const std::string title =
          renderer.truncatedText(UI_10_FONT_ID, recommendations[i].title.c_str(), textWidth, EpdFontFamily::BOLD);
      const int titleTextY = recommendations[i].author.empty()
                                 ? row.y + (row.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2
                                 : row.y + std::max(7, (row.height - renderer.getLineHeight(UI_10_FONT_ID) -
                                                        renderer.getLineHeight(SMALL_FONT_ID) - 2) /
                                                           2);
      renderer.drawText(UI_10_FONT_ID, textX, titleTextY, title.c_str(), true, EpdFontFamily::BOLD);
      if (!recommendations[i].author.empty()) {
        const std::string author = renderer.truncatedText(SMALL_FONT_ID, recommendations[i].author.c_str(), textWidth);
        renderer.drawText(SMALL_FONT_ID, textX, titleTextY + renderer.getLineHeight(UI_10_FONT_ID) + 2, author.c_str());
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
