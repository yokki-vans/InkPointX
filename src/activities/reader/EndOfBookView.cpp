#include "EndOfBookView.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
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

bool isUsableBitmap(const std::string& path, int* height = nullptr) {
  if (path.empty() || !Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("ENDCOVER", path, file)) return false;
  Bitmap bitmap(file);
  const bool usable = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0;
  if (usable && height) *height = bitmap.getHeight();
  file.close();
  return usable;
}

std::string findCachedThumbnail(const std::string& cachePath, const int preferredHeight) {
  if (cachePath.empty()) return {};
  HalFile directory = Storage.open(cachePath.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return {};
  }

  std::string bestPath;
  int bestDistance = std::numeric_limits<int>::max();
  char name[192];
  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(name, sizeof(name));
    entry.close();
    std::string filename(name);
    const size_t slash = filename.find_last_of('/');
    if (slash != std::string::npos) filename.erase(0, slash + 1);
    if (filename.rfind("thumb_", 0) != 0 || filename.size() < 11 ||
        filename.compare(filename.size() - 4, 4, ".bmp") != 0) {
      continue;
    }
    const std::string candidate = cachePath + "/" + filename;
    int height = 0;
    if (!isUsableBitmap(candidate, &height)) continue;
    const int distance = std::abs(height - preferredHeight);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestPath = candidate;
    }
  }
  directory.close();
  return bestPath;
}

std::string resolveCoverPath(const std::string& bookPath, const std::string& coverTemplate, const int preferredHeight) {
  const std::array<int, 5> heights = {preferredHeight, 402, 300, 226, 540};
  if (!coverTemplate.empty()) {
    for (const int height : heights) {
      const std::string candidate = UITheme::getCoverThumbPath(coverTemplate, height);
      if (isUsableBitmap(candidate)) return candidate;
    }
    if (isUsableBitmap(coverTemplate)) return coverTemplate;
  }

  const std::string cachePath = getBookCachePath(bookPath);
  if (cachePath.empty()) return {};
  for (const int height : heights) {
    const std::string candidate = cachePath + "/thumb_" + std::to_string(height) + ".bmp";
    if (isUsableBitmap(candidate)) return candidate;
  }
  const std::string cachedThumbnail = findCachedThumbnail(cachePath, preferredHeight);
  if (!cachedThumbnail.empty()) return cachedThumbnail;
  const std::array<std::string, 2> fullCovers = {cachePath + "/cover.bmp", cachePath + "/cover_crop.bmp"};
  const auto cover =
      std::find_if(fullCovers.begin(), fullCovers.end(), [](const std::string& path) { return isUsableBitmap(path); });
  return cover == fullCovers.end() ? std::string{} : *cover;
}

std::string prepareCoverPath(const std::string& bookPath, const std::string& coverTemplate, const int preferredHeight) {
  std::string cover = resolveCoverPath(bookPath, coverTemplate, preferredHeight);
  if (!cover.empty()) return cover;

  // Generate only from an existing book index. Building a missing index here
  // would turn a celebratory screen into a long blocking operation.
  if (FsHelpers::hasEpubExtension(bookPath) || FsHelpers::hasFb2Extension(bookPath) ||
      FsHelpers::hasPdfExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    if (epub.load(false, true)) {
      const std::string target = epub.getThumbBmpPath(preferredHeight);
      if (Storage.exists(target.c_str()) && !isUsableBitmap(target)) Storage.remove(target.c_str());
      if (epub.generateThumbBmp(preferredHeight) && isUsableBitmap(target)) return target;
    }
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (xtc.load()) {
      const std::string target = xtc.getThumbBmpPath(preferredHeight);
      if (Storage.exists(target.c_str()) && !isUsableBitmap(target)) Storage.remove(target.c_str());
      if (xtc.generateThumbBmp(preferredHeight) && isUsableBitmap(target)) return target;
    }
  } else if (FsHelpers::hasTxtExtension(bookPath) || FsHelpers::hasMarkdownExtension(bookPath)) {
    Txt txt(bookPath, "/.crosspoint");
    if (txt.load() && txt.generateCoverBmp() && isUsableBitmap(txt.getCoverBmpPath())) return txt.getCoverBmpPath();
  }
  return {};
}

bool restoreCoverTile(const GfxRenderer& renderer, const Rect& slot, EndOfBookView::CoverTileCache& cache) {
  return cache.data && cache.size > 0 && cache.x == slot.x && cache.y == slot.y && cache.width == slot.width &&
         cache.height == slot.height &&
         renderer.copyBufferToRegion(slot.x, slot.y, slot.width, slot.height, cache.data.get(), cache.size);
}

void saveCoverTile(const GfxRenderer& renderer, const Rect& slot, EndOfBookView::CoverTileCache& cache) {
  const size_t size = renderer.getRegionByteSize(slot.x, slot.y, slot.width, slot.height);
  if (size == 0) return;
  if (!cache.data || cache.size != size) {
    cache.data.reset(new (std::nothrow) uint8_t[size]);
    cache.size = cache.data ? size : 0;
  }
  if (!cache.data ||
      !renderer.copyRegionToBuffer(slot.x, slot.y, slot.width, slot.height, cache.data.get(), cache.size)) {
    cache.data.reset();
    cache.size = 0;
    return;
  }
  cache.x = slot.x;
  cache.y = slot.y;
  cache.width = slot.width;
  cache.height = slot.height;
}

void drawCoverTile(const GfxRenderer& renderer, const Rect& slot, const std::string& coverPath,
                   EndOfBookView::CoverTileCache& cache, const int radius) {
  if (restoreCoverTile(renderer, slot, cache)) return;

  renderer.fillRoundedRect(slot.x, slot.y, slot.width, slot.height, radius, Color::White);
  bool drawn = false;
  if (!coverPath.empty()) {
    HalFile file;
    if (Storage.openFileForRead("ENDCOVER", coverPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        float scale = std::min(static_cast<float>(slot.width) / bitmap.getWidth(),
                               static_cast<float>(slot.height) / bitmap.getHeight());
        if (!bitmap.is1Bit()) scale = std::min(1.0f, scale);
        const int coverWidth = std::max(1, static_cast<int>(std::round(bitmap.getWidth() * scale)));
        const int coverHeight = std::max(1, static_cast<int>(std::round(bitmap.getHeight() * scale)));
        const int coverX = slot.x + (slot.width - coverWidth) / 2;
        const int coverY = slot.y + (slot.height - coverHeight) / 2;
        renderer.fillRect(coverX, coverY, coverWidth, coverHeight, false);
        if (bitmap.is1Bit()) {
          renderer.drawBitmap1Bit(bitmap, coverX, coverY, coverWidth, coverHeight, true);
        } else {
          renderer.drawBitmap(bitmap, coverX, coverY, coverWidth, coverHeight);
        }
        renderer.maskRoundedRectOutsideCorners(coverX, coverY, coverWidth, coverHeight, radius, Color::White);
        renderer.drawRoundedRect(coverX, coverY, coverWidth, coverHeight, 1, radius, true);
        drawn = true;
      }
      file.close();
    }
  }

  if (!drawn) {
    renderer.drawRoundedRect(slot.x, slot.y, slot.width, slot.height, 1, radius, true);
    const int bandHeight = std::max(14, slot.height / 4);
    renderer.fillRect(slot.x + 1, slot.y + 1, slot.width - 2, bandHeight, true);
    renderer.maskRoundedRectOutsideCorners(slot.x, slot.y, slot.width, slot.height, radius, Color::White);
    const int centerX = slot.x + slot.width / 2;
    const int firstLineY = slot.y + bandHeight + std::max(12, slot.height / 5);
    const int longLine = std::max(18, slot.width * 3 / 5);
    const int shortLine = std::max(14, slot.width * 2 / 5);
    renderer.drawLine(centerX - longLine / 2, firstLineY, centerX + longLine / 2, firstLineY, true);
    renderer.drawLine(centerX - shortLine / 2, firstLineY + 8, centerX + shortLine / 2, firstLineY + 8, true);
    renderer.drawLine(centerX - longLine / 2, firstLineY + 16, centerX + longLine / 2, firstLineY + 16, true);
    renderer.drawRoundedRect(slot.x, slot.y, slot.width, slot.height, 1, radius, true);
  }
  saveCoverTile(renderer, slot, cache);
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
  candidate.coverBmpPath = prepareCoverPath(candidate.path, candidate.coverBmpPath, 112);
  recommendations.push_back(std::move(candidate));
}

void EndOfBookView::prepare(const std::string& currentPath, const std::string& title, const std::string& author,
                            const BookReadingStats& stats) {
  if (prepared) return;
  prepared = true;
  finishedTitle = title.empty() ? titleFromPath(currentPath) : title;
  finishedAuthor = author;
  finishedCoverPath.clear();
  readingSeconds = stats.totalReadingSeconds;
  selectedIndex = 0;
  recommendations.clear();
  recommendations.reserve(MAX_RECOMMENDATIONS);
  finishedCoverCache = {};
  recommendationCoverCaches.clear();
  recommendationCoverCaches.resize(MAX_RECOMMENDATIONS);

  // A recently touched favourite is the strongest local signal available on
  // an offline reader.  Then use the remaining favourites and recent books.
  const auto& recent = RECENT_BOOKS.getBooks();
  const auto& favourites = FAVORITE_BOOKS.getBooks();
  const auto recentCurrent =
      std::find_if(recent.begin(), recent.end(), [&](const RecentBook& book) { return book.path == currentPath; });
  const auto favouriteCurrent = std::find_if(favourites.begin(), favourites.end(),
                                             [&](const RecentBook& book) { return book.path == currentPath; });
  const std::string currentCoverTemplate = recentCurrent != recent.end()          ? recentCurrent->coverBmpPath
                                           : favouriteCurrent != favourites.end() ? favouriteCurrent->coverBmpPath
                                                                                  : std::string{};
  finishedCoverPath = prepareCoverPath(currentPath, currentCoverTemplate, 180);
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

  const int headerY = compact ? 6 : 12;
  drawCentered(renderer, SCRIPT_SMALL_FONT_ID, margin, innerWidth, headerY, tr(STR_END_OF_BOOK));

  const int heroTop = headerY + renderer.getLineHeight(SCRIPT_SMALL_FONT_ID) + (compact ? 7 : 10);
  const int heroHeight = compact ? 154 : 180;
  const int heroCoverWidth = heroHeight * 2 / 3;
  const Rect heroCover{margin + 6, heroTop, heroCoverWidth, heroHeight};
  drawCoverTile(renderer, heroCover, finishedCoverPath, finishedCoverCache, 10);

  const int detailsX = heroCover.x + heroCover.width + 16;
  const int detailsWidth = width - margin - 6 - detailsX;
  int detailsY = heroTop + 3;
  detailsY = drawCenteredWrapped(renderer, UI_10_FONT_ID, detailsX, detailsWidth, detailsY, finishedTitle.c_str(),
                                 compact ? 2 : 3, EpdFontFamily::BOLD);
  if (!finishedAuthor.empty()) {
    detailsY += 3;
    const auto authorLines = renderer.wrappedText(SMALL_FONT_ID, finishedAuthor.c_str(), detailsWidth, 2);
    for (const auto& line : authorLines) {
      drawCentered(renderer, SMALL_FONT_ID, detailsX, detailsWidth, detailsY, line.c_str());
      detailsY += renderer.getLineHeight(SMALL_FONT_ID);
    }
  }

  char duration[32];
  BookReadingStats::formatDuration(readingSeconds, duration, sizeof(duration));
  const std::string summary = std::string("100% · ") + duration;
  const int summaryY = std::max(detailsY + 8, heroTop + heroHeight - renderer.getLineHeight(SMALL_FONT_ID) - 5);
  drawCentered(renderer, SMALL_FONT_ID, detailsX, detailsWidth, summaryY, summary.c_str(), EpdFontFamily::BOLD);

  const int sectionY = heroTop + heroHeight + (compact ? 12 : 17);
  const int sectionTextWidth = renderer.getTextWidth(SCRIPT_SMALL_FONT_ID, tr(STR_NEXT_FIELD));
  const int sectionTextX = (width - sectionTextWidth) / 2;
  const int sectionLineY = sectionY + renderer.getLineHeight(SCRIPT_SMALL_FONT_ID) / 2;
  renderer.drawLine(margin + 8, sectionLineY, sectionTextX - 12, sectionLineY, true);
  renderer.drawText(SCRIPT_SMALL_FONT_ID, sectionTextX, sectionY, tr(STR_NEXT_FIELD));
  renderer.drawLine(sectionTextX + sectionTextWidth + 12, sectionLineY, width - margin - 8, sectionLineY, true);
  const int rowsTop = sectionY + renderer.getLineHeight(SCRIPT_SMALL_FONT_ID) + (compact ? 5 : 8);

  if (recommendations.empty()) {
    const int emptyHeight = std::max(42, contentBottom - rowsTop - 8);
    drawCenteredWrapped(renderer, SMALL_FONT_ID, margin + 28, innerWidth - 56,
                        rowsTop + std::max(8, (emptyHeight - renderer.getLineHeight(SMALL_FONT_ID) * 2) / 3),
                        tr(STR_OPEN_LIBRARY_HINT), 2);
  } else {
    const int count = static_cast<int>(recommendations.size());
    const int available = std::max(1, contentBottom - rowsTop);
    const int maxRowHeight = count == 1 ? 190 : count == 2 ? 170 : 158;
    const int rowHeight = std::clamp(available / count, compact ? 94 : 112, compact ? maxRowHeight - 12 : maxRowHeight);
    for (size_t i = 0; i < recommendations.size(); ++i) {
      const bool selected = i == selectedIndex;
      const int y = rowsTop + static_cast<int>(i) * rowHeight;
      const Rect row{margin, y, innerWidth, rowHeight};
      if (selected) {
        GUI.drawSelection(renderer, Rect{row.x, row.y + 4, row.width, row.height - 8});
      } else if (i > 0 && i - 1 != selectedIndex) {
        renderer.drawLine(row.x + 12, row.y, row.x + row.width - 12, row.y, true);
      }

      const int coverHeight = std::min(compact ? 94 : 112, row.height - 18);
      const int coverWidth = std::max(42, coverHeight * 2 / 3);
      const Rect cover{row.x + 14, row.y + (row.height - coverHeight) / 2, coverWidth, coverHeight};
      drawCoverTile(renderer, cover, recommendations[i].coverBmpPath, recommendationCoverCaches[i], 6);
      const int chevronX = row.x + row.width - 32;
      renderer.drawIcon(LucideChevronRight24, chevronX, row.y + (row.height - 24) / 2, 24, 24);

      const int textX = cover.x + cover.width + 13;
      const int textWidth = chevronX - textX - 8;
      const std::string title = renderer.truncatedText(UI_10_FONT_ID, recommendations[i].title.c_str(), textWidth,
                                                       selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      const int titleTextY = recommendations[i].author.empty()
                                 ? row.y + (row.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2
                                 : row.y + std::max(5, (row.height - renderer.getLineHeight(UI_10_FONT_ID) -
                                                        renderer.getLineHeight(MICRO_FONT_ID) - 1) /
                                                           2);
      renderer.drawText(UI_10_FONT_ID, textX, titleTextY, title.c_str(), true,
                        selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      if (!recommendations[i].author.empty()) {
        const std::string author = renderer.truncatedText(MICRO_FONT_ID, recommendations[i].author.c_str(), textWidth);
        renderer.drawText(MICRO_FONT_ID, textX, titleTextY + renderer.getLineHeight(UI_10_FONT_ID) + 1, author.c_str());
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
