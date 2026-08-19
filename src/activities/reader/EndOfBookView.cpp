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

  int cursorY = compact ? 8 : 20;
  drawCentered(renderer, SCRIPT_SMALL_FONT_ID, margin, innerWidth, cursorY, tr(STR_END_OF_BOOK));
  cursorY += renderer.getLineHeight(SCRIPT_SMALL_FONT_ID) + (compact ? 8 : 14);

  cursorY = drawCenteredWrapped(renderer, UI_10_FONT_ID, margin + 14, innerWidth - 28, cursorY,
                                finishedTitle.c_str(), compact ? 1 : 2, EpdFontFamily::BOLD);
  if (!finishedAuthor.empty()) {
    cursorY += compact ? 0 : 2;
    const std::string author = renderer.truncatedText(SMALL_FONT_ID, finishedAuthor.c_str(), innerWidth - 48);
    drawCentered(renderer, SMALL_FONT_ID, margin + 24, innerWidth - 48, cursorY, author.c_str());
    cursorY += renderer.getLineHeight(SMALL_FONT_ID);
  }

  char duration[32];
  BookReadingStats::formatDuration(readingSeconds, duration, sizeof(duration));
  const std::string summary = std::string("100% · ") + duration;
  cursorY += compact ? 5 : 9;
  drawCentered(renderer, SMALL_FONT_ID, margin, innerWidth, cursorY, summary.c_str(), EpdFontFamily::BOLD);
  cursorY += renderer.getLineHeight(SMALL_FONT_ID) + (compact ? 8 : 13);

  renderer.drawLine(margin + 8, cursorY, width - margin - 8, cursorY, true);
  cursorY += compact ? 8 : 12;
  drawCentered(renderer, SMALL_FONT_ID, margin, innerWidth, cursorY, tr(STR_NEXT_FIELD), EpdFontFamily::BOLD);
  const int rowsTop = cursorY + renderer.getLineHeight(SMALL_FONT_ID) + (compact ? 4 : 8);

  if (recommendations.empty()) {
    const int emptyHeight = std::max(42, contentBottom - rowsTop - 8);
    drawCenteredWrapped(renderer, SMALL_FONT_ID, margin + 28, innerWidth - 56,
                        rowsTop + std::max(8, (emptyHeight - renderer.getLineHeight(SMALL_FONT_ID) * 2) / 3),
                        tr(STR_OPEN_LIBRARY_HINT), 2);
  } else {
    const int count = static_cast<int>(recommendations.size());
    const int available = std::max(1, contentBottom - rowsTop);
    const int rowHeight = std::clamp(available / count, compact ? 44 : 58, compact ? 54 : 70);
    for (size_t i = 0; i < recommendations.size(); ++i) {
      const bool selected = i == selectedIndex;
      const int y = rowsTop + static_cast<int>(i) * rowHeight;
      const Rect row{margin, y, innerWidth, rowHeight};
      if (selected) {
        renderer.drawRoundedRect(row.x, row.y + 2, row.width, row.height - 4, 1, 10, true);
        renderer.fillRoundedRect(row.x + 7, row.y + 11, 3, row.height - 22, 2, Color::Black);
      } else if (i > 0 && i - 1 != selectedIndex) {
        renderer.drawLine(row.x + 18, row.y, row.x + row.width - 18, row.y, true);
      }

      const int iconSize = 32;
      const int iconX = row.x + 18;
      const int iconY = row.y + (row.height - iconSize) / 2;
      renderer.drawIcon(LucideBookOpen32, iconX, iconY, iconSize, iconSize);
      const int chevronX = row.x + row.width - 32;
      renderer.drawIcon(LucideChevronRight24, chevronX, row.y + (row.height - 24) / 2, 24, 24);

      const int textX = iconX + iconSize + 10;
      const int textWidth = chevronX - textX - 8;
      const std::string title =
          renderer.truncatedText(SMALL_FONT_ID, recommendations[i].title.c_str(), textWidth, EpdFontFamily::BOLD);
      const int titleTextY = recommendations[i].author.empty()
                                 ? row.y + (row.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2
                                 : row.y + std::max(5, (row.height - renderer.getLineHeight(SMALL_FONT_ID) -
                                                        renderer.getLineHeight(MICRO_FONT_ID) - 1) /
                                                           2);
      renderer.drawText(SMALL_FONT_ID, textX, titleTextY, title.c_str(), true, EpdFontFamily::BOLD);
      if (!recommendations[i].author.empty()) {
        const std::string author = renderer.truncatedText(MICRO_FONT_ID, recommendations[i].author.c_str(), textWidth);
        renderer.drawText(MICRO_FONT_ID, textX, titleTextY + renderer.getLineHeight(SMALL_FONT_ID) + 1,
                          author.c_str());
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
