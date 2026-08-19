#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "BidiUtils.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/NetworkModeSelectionActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/settings/SettingsActivity.h"
#include "components/UITheme.h"
#include "components/icons/lucide_ui.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr int HOME_CONTENT_MARGIN = 18;
constexpr int HOME_COVER_SOURCE_HEIGHT = 402;
constexpr int HOME_COVER_MIN_HEIGHT = 160;
constexpr int HOME_COVER_RADIUS = 14;
constexpr int HOME_HEADER_TO_CONTENT_GAP = 12;
constexpr int HOME_TITLE_TO_COVER_GAP = 12;
constexpr int HOME_COVER_TO_AUTHOR_GAP = 10;
constexpr int HOME_METADATA_GAP = 18;
constexpr int HOME_PROGRESS_BAR_GAP = 8;
constexpr int HOME_PROGRESS_BAR_THICKNESS = 6;
constexpr int HOME_ACTION_SIDE_MARGIN = 20;
constexpr int HOME_DOTS_TOP_OFFSET = 50;
constexpr int HOME_DOTS_CLEARANCE = 22;
constexpr int HOME_PLACEHOLDER_TEXT_MARGIN = 22;
constexpr int HOME_PLACEHOLDER_TITLE_PADDING = 28;
constexpr int HOME_PLACEHOLDER_AUTHOR_PADDING = 28;

struct CachedHomeDetails {
  bool valid = false;
  std::string bookPath;
  std::string coverPath;
  std::string cachePath;
  uint8_t progressPercent = 0;
  uint32_t readingSeconds = 0;
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  bool completed = false;
};

CachedHomeDetails cachedHomeDetails;

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t clampPageCount(const double value) {
  if (value <= 0.0) return 0;
  const double maximum = static_cast<double>(std::numeric_limits<uint32_t>::max());
  return static_cast<uint32_t>(std::min(value, maximum));
}

bool isUsableBitmap(const std::string& path) {
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("HOME", path, file)) return false;
  Bitmap bitmap(file);
  const bool usable = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0;
  file.close();
  return usable;
}

bool prepareHomeThumb(const Epub& epub, const std::string& path, const int targetHeight) {
  if (isUsableBitmap(path)) return true;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  if (!epub.generateThumbBmp(targetHeight)) return false;
  return isUsableBitmap(path);
}

std::string filenameWithoutExtension(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t length = dot == std::string::npos || dot <= start ? std::string::npos : dot - start;
  return path.substr(start, length);
}

int homeAuthorFontId(const GfxRenderer& renderer, const char* text) {
  // A downloaded accent family has a real 16 pt RTL face. The built-in Caveat
  // cut does not, and duplicating Noto Hebrew/Arabic in a second embedded size
  // would consume almost 0.5 MB of the OTA partition. Reuse the compact 12 pt
  // structural face only for those scripts; all Caveat-supported authors keep
  // the smaller handwritten treatment.
  if (renderer.isSdCardFont(SCRIPT_SMALL_FONT_ID) || !text) return SCRIPT_SMALL_FONT_ID;
  auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if ((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
      return UI_12_FONT_ID;
    }
  }
  return SCRIPT_SMALL_FONT_ID;
}

bool resolveHomeMetadataVisibility(const uint8_t mode, const bool hasCover) {
  if (mode == CrossPointSettings::HOME_METADATA_SHOW) return true;
  if (mode == CrossPointSettings::HOME_METADATA_HIDE) return false;
  return !hasCover;
}

struct HomeBookLayout {
  int titleTop = 0;
  int coverTop = 0;
  int coverSlotHeight = HOME_COVER_MIN_HEIGHT;
  int coverMaxWidth = 0;
  int authorTop = 0;
  int detailsTop = 0;
};

HomeBookLayout calculateHomeBookLayout(const GfxRenderer& renderer, const int titleLineCount, const bool showTitle,
                                       const bool showAuthor, const int authorLineHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int titleBlockHeight = showTitle ? std::max(1, titleLineCount) * renderer.getLineHeight(UI_14_FONT_ID) : 0;
  const int captionLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int progressBlockHeight = HOME_PROGRESS_BAR_THICKNESS + HOME_PROGRESS_BAR_GAP + captionLineHeight;
  const int dotsY = renderer.getScreenHeight() - metrics.buttonHintsHeight - HOME_DOTS_TOP_OFFSET;
  const int safeDetailsBottom = dotsY - HOME_DOTS_CLEARANCE;

  HomeBookLayout layout;
  layout.titleTop = metrics.topPadding + metrics.headerHeight + HOME_HEADER_TO_CONTENT_GAP;
  layout.coverTop = layout.titleTop + (showTitle ? titleBlockHeight + HOME_TITLE_TO_COVER_GAP : 0);
  layout.detailsTop = safeDetailsBottom - progressBlockHeight;
  layout.authorTop = showAuthor ? layout.detailsTop - HOME_METADATA_GAP - authorLineHeight : 0;

  // The metadata rows are fixed to stable anchors: title at the top, reading
  // progress at the bottom and author immediately above it. The cover owns the
  // exact remaining area. This keeps every visibility combination balanced,
  // including wide artwork that cannot consume all available height.
  const int coverBottom =
      showAuthor ? layout.authorTop - HOME_COVER_TO_AUTHOR_GAP : layout.detailsTop - HOME_METADATA_GAP;
  layout.coverSlotHeight = std::max(HOME_COVER_MIN_HEIGHT, coverBottom - layout.coverTop);
  // Let the artwork use the whole content box. The slot height remains the
  // primary limit for normal portrait covers, so layouts with metadata keep
  // their previous visual scale. When title/author rows are hidden, however,
  // the cover can now consume every newly released pixel instead of hitting
  // the old arbitrary +30/+40 px width caps and leaving a visible gap.
  layout.coverMaxWidth = renderer.getScreenWidth() - HOME_CONTENT_MARGIN * 2;
  return layout;
}

}  // namespace

void HomeActivity::invalidateDetailsCache() { cachedHomeDetails = {}; }

void HomeActivity::onEnter() {
  Activity::onEnter();
  recentBooks.clear();
  recentBooks.reserve(1);
  const auto& books = RECENT_BOOKS.getBooks();
  const auto availableBook = std::find_if(books.begin(), books.end(),
                                          [](const RecentBook& book) { return !RecentBooksStore::isMissing(book); });
  if (availableBook != books.end()) {
    recentBooks.push_back(*availableBook);
  }
  applyInitialSelection();
  // Never expose a half-populated Home frame. The common path below only
  // opens cached thumbnail/progress data; legacy books are migrated before
  // this single complete frame is presented.
  if (pageIndex == 0) loadRecentBookDetails();
  requestUpdateAndWait();
}

void HomeActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  homeCoverPath.clear();
  homeCachePath.clear();
  coverRegionCache.reset();
  coverRegionCacheSize = 0;
  readingSummary = {};
  recentDetailsLoaded = false;
}

void HomeActivity::loadRecentBookDetails() {
  recentDetailsLoaded = true;
  readingSummary = {};
  homeCoverPath.clear();
  homeCachePath.clear();
  coverRegionCache.reset();
  coverRegionCacheSize = 0;
  if (recentBooks.empty()) return;

  const RecentBook& book = recentBooks.front();
  if (cachedHomeDetails.valid && cachedHomeDetails.bookPath == book.path) {
    homeCoverPath = cachedHomeDetails.coverPath;
    homeCachePath = cachedHomeDetails.cachePath;
    readingSummary.progressPercent = cachedHomeDetails.progressPercent;
    readingSummary.readingSeconds = cachedHomeDetails.readingSeconds;
    readingSummary.currentPage = cachedHomeDetails.currentPage;
    readingSummary.totalPages = cachedHomeDetails.totalPages;
    if (cachedHomeDetails.completed && readingSummary.totalPages > 0) {
      readingSummary.progressPercent = 100;
      readingSummary.currentPage = readingSummary.totalPages;
    }
    return;
  }
  bool completed = false;
  ScopedCleanup persistDetails{[this, &book, &completed] {
    cachedHomeDetails.valid = true;
    cachedHomeDetails.bookPath = book.path;
    cachedHomeDetails.coverPath = homeCoverPath;
    cachedHomeDetails.cachePath = homeCachePath;
    cachedHomeDetails.progressPercent = readingSummary.progressPercent;
    cachedHomeDetails.readingSeconds = readingSummary.readingSeconds;
    cachedHomeDetails.currentPage = readingSummary.currentPage;
    cachedHomeDetails.totalPages = readingSummary.totalPages;
    cachedHomeDetails.completed = completed;
  }};
  const std::string displayTitle = book.title.empty() ? filenameWithoutExtension(book.path) : book.title;
  // Generate the largest thumbnail needed by the cover-present AUTO layout.
  // If generation fails, render() detects the missing bitmap and switches to
  // the no-cover layout where the title (and available author) are visible.
  const bool showTitle = resolveHomeMetadataVisibility(SETTINGS.homeBookTitleMode, true);
  const bool showAuthor = resolveHomeMetadataVisibility(SETTINGS.homeBookAuthorMode, true) && !book.author.empty();
  const int titleLineCount =
      showTitle ? static_cast<int>(renderer
                                       .wrappedText(UI_14_FONT_ID, displayTitle.c_str(),
                                                    renderer.getScreenWidth() - HOME_CONTENT_MARGIN * 2, 2,
                                                    EpdFontFamily::BOLD)
                                       .size())
                : 0;
  const int authorLineHeight = showAuthor ? renderer.getLineHeight(homeAuthorFontId(renderer, book.author.c_str())) : 0;
  const int coverTargetHeight =
      calculateHomeBookLayout(renderer, titleLineCount, showTitle, showAuthor, authorLineHeight).coverSlotHeight;
  const bool epubCompatible = FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasFb2Extension(book.path) ||
                              FsHelpers::hasPdfExtension(book.path);

  const std::string cachePath = getBookCachePath(book.path);
  homeCachePath = cachePath;
  // RecentBooksStore already persists a height-template for the cover. Resolve
  // an existing thumbnail directly instead of loading the EPUB just to ask it
  // for the same path.
  if (!book.coverBmpPath.empty()) {
    const std::array<int, 4> fallbackHeights = {coverTargetHeight, HOME_COVER_SOURCE_HEIGHT, 300, 226};
    for (const int height : fallbackHeights) {
      const std::string candidate = UITheme::getCoverThumbPath(book.coverBmpPath, height);
      if (isUsableBitmap(candidate)) {
        homeCoverPath = candidate;
        break;
      }
    }
  }

  if (cachePath.empty()) return;
  const BookReadingStats stats = BookReadingStats::load(cachePath);
  completed = stats.isCompleted;
  readingSummary.readingSeconds = stats.totalReadingSeconds;

  if (!epubCompatible) {
    readingSummary.currentPage = stats.totalPagesTurned;
    return;
  }

  HalFile progressFile;
  if (!Storage.openFileForRead("HOME", cachePath + "/progress.bin", progressFile)) return;
  // Byte 6 has held a denormalized whole-book percentage since 2.2.0. New
  // writes also include whole-book page estimates in bytes 7..14. Reading this
  // lightweight record makes the normal Home path independent of EPUB parsing.
  uint8_t data[15] = {};
  const int bytesRead = progressFile.read(data, sizeof(data));
  progressFile.close();
  if (bytesRead >= 7 && data[6] <= 100) {
    readingSummary.progressPercent = data[6];
    if (bytesRead >= 15) {
      readingSummary.currentPage = readLe32(data + 7);
      readingSummary.totalPages = readLe32(data + 11);
    }
    // Older builds could mark a book complete while leaving the denormalized
    // page estimate one page short (for example, 752 / 753). Completion is the
    // authoritative state, so repair that stale presentation immediately.
    if (completed && readingSummary.totalPages > 0) {
      readingSummary.progressPercent = 100;
      readingSummary.currentPage = readingSummary.totalPages;
    }
    if (!homeCoverPath.empty()) return;
  } else if (bytesRead != 4 && bytesRead != 6) {
    return;
  }

  // Legacy progress or a missing thumbnail needs the indexed metadata once.
  // The next reader save writes the extended lightweight record, while an
  // existing fallback thumbnail is reused without generating a new size.
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) return;

  if (homeCoverPath.empty()) {
    const std::string requestedThumb = epub.getThumbBmpPath(coverTargetHeight);
    if (prepareHomeThumb(epub, requestedThumb, coverTargetHeight)) {
      homeCoverPath = requestedThumb;
    } else {
      const std::array<std::string, 4> fallbackCovers = {epub.getCoverBmpPath(false),
                                                         epub.getThumbBmpPath(HOME_COVER_SOURCE_HEIGHT),
                                                         epub.getThumbBmpPath(300), epub.getThumbBmpPath(226)};
      const auto fallback = std::find_if(fallbackCovers.begin(), fallbackCovers.end(),
                                         [](const std::string& path) { return isUsableBitmap(path); });
      if (fallback != fallbackCovers.end()) homeCoverPath = *fallback;
    }
  }

  const uint16_t spineIndex = readLe16(data);
  uint16_t chapterPage = readLe16(data + 2);
  if (chapterPage == UINT16_MAX) chapterPage = 0;
  const uint16_t chapterPageCount = bytesRead >= 6 ? readLe16(data + 4) : 0;
  if (spineIndex >= epub.getSpineItemsCount()) return;

  const float sectionRead =
      chapterPageCount > 0
          ? std::min(1.0f, static_cast<float>(std::min<uint16_t>(chapterPage + 1, chapterPageCount)) / chapterPageCount)
          : 0.0f;
  const float bookProgress = std::clamp(epub.calculateProgress(spineIndex, sectionRead), 0.0f, 1.0f);
  readingSummary.progressPercent = static_cast<uint8_t>(bookProgress * 100.0f + 0.5f);

  if (chapterPageCount == 0) return;
  const size_t previousBytes = spineIndex > 0 ? epub.getCumulativeSpineItemSize(spineIndex - 1) : 0;
  const size_t cumulativeBytes = epub.getCumulativeSpineItemSize(spineIndex);
  const size_t chapterBytes = cumulativeBytes > previousBytes ? cumulativeBytes - previousBytes : 0;
  const size_t bookBytes = epub.getBookSize();
  if (chapterBytes == 0 || bookBytes == 0) return;

  const double bytesPerPage = static_cast<double>(chapterBytes) / chapterPageCount;
  readingSummary.totalPages = clampPageCount(std::ceil(static_cast<double>(bookBytes) / bytesPerPage));
  const double readBytes = static_cast<double>(previousBytes) + static_cast<double>(chapterBytes) * sectionRead;
  readingSummary.currentPage = clampPageCount(std::round(readBytes / bytesPerPage));
  if (readingSummary.totalPages > 0) {
    readingSummary.currentPage = std::clamp<uint32_t>(readingSummary.currentPage, 1, readingSummary.totalPages);
    if (completed) {
      readingSummary.progressPercent = 100;
      readingSummary.currentPage = readingSummary.totalPages;
    }
  }
}

void HomeActivity::applyInitialSelection() {
  pageIndex = 0;
  selectedIndex = 0;
  switch (initialMenuItem) {
    case HomeMenuItem::LIBRARY:
      pageIndex = 1;
      selectedIndex = 0;
      break;
    case HomeMenuItem::FILE_BROWSER:
      pageIndex = 1;
      selectedIndex = 1;
      break;
    case HomeMenuItem::GALLERY:
      pageIndex = 1;
      selectedIndex = 2;
      break;
    case HomeMenuItem::FAVORITES:
      pageIndex = 1;
      selectedIndex = 3;
      break;
    case HomeMenuItem::READING_STATS:
      pageIndex = 1;
      selectedIndex = 4;
      break;
    case HomeMenuItem::FILE_TRANSFER:
      pageIndex = 1;
      selectedIndex = 5;
      break;
    case HomeMenuItem::OPDS_BROWSER:
      pageIndex = 2;
      selectedIndex = 5;
      break;
    case HomeMenuItem::SETTINGS_MENU:
      pageIndex = 2;
      selectedIndex = 0;
      break;
    case HomeMenuItem::SETTINGS_POWER:
      pageIndex = 2;
      selectedIndex = 1;
      break;
    case HomeMenuItem::SETTINGS_READING:
      pageIndex = 2;
      selectedIndex = 2;
      break;
    case HomeMenuItem::SETTINGS_CONTROLS:
      pageIndex = 2;
      selectedIndex = 3;
      break;
    case HomeMenuItem::SETTINGS_LIBRARY:
      pageIndex = 2;
      selectedIndex = 4;
      break;
    case HomeMenuItem::SETTINGS_NETWORK:
      pageIndex = 2;
      selectedIndex = 5;
      break;
    case HomeMenuItem::SETTINGS_SYSTEM:
      pageIndex = 2;
      selectedIndex = 6;
      break;
    case HomeMenuItem::RECENTS:
    case HomeMenuItem::NONE:
      break;
  }
}

int HomeActivity::pageItemCount() const {
  if (pageIndex == 0) return 1;
  if (pageIndex == 1) return 8;
  return SettingsActivity::CATEGORY_COUNT;
}

void HomeActivity::openSelection() {
  if (pageIndex == 0) {
    if (!recentBooks.empty()) {
      activityManager.goToReader(recentBooks[0].path);
    } else {
      activityManager.goToLibrary();
    }
    return;
  }

  if (pageIndex == 1) {
    switch (selectedIndex) {
      case 0:
        activityManager.goToLibrary();
        return;
      case 1:
        activityManager.goToFileBrowser();
        return;
      case 2:
        activityManager.goToGallery();
        return;
      case 3:
        activityManager.goToFavorites();
        return;
      case 4:
        activityManager.goToReadingStats();
        return;
      case 5:
        activityManager.goToFileTransfer(NetworkMode::JOIN_NETWORK);
        return;
      case 6:
        activityManager.goToFileTransfer(NetworkMode::CONNECT_CALIBRE);
        return;
      case 7:
        activityManager.goToFileTransfer(NetworkMode::CREATE_HOTSPOT);
        return;
      default:
        return;
    }
  }

  activityManager.goToSettings(selectedIndex);
}

void HomeActivity::loop() {
  const auto changePage = [this](const int delta) {
    rememberedSelection[pageIndex] = selectedIndex;
    pageIndex = (pageIndex + delta + PAGE_COUNT) % PAGE_COUNT;
    // Clamp on the way in: a page's item count can change between visits.
    selectedIndex = std::min(rememberedSelection[pageIndex], std::max(0, pageItemCount() - 1));
    // Populate the state before scheduling the frame: Now Reading must appear
    // as one complete image, never as shell -> delayed cover/progress.
    if (pageIndex == 0 && !recentDetailsLoaded) loadRecentBookDetails();
    requestUpdate();
  };

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    changePage(1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    changePage(-1);
    return;
  }

  const int count = pageItemCount();
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex + 1) % count;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex + count - 1) % count;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelection();
}

void HomeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (pageIndex == 0) {
    // The greeting is the one header set in the handwritten accent face —
    // it is a warm phrase, not navigation. The empty-title drawHeader still
    // paints the battery cluster and the rule; the script baseline is aligned
    // to where the structural header's baseline would sit.
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, nullptr);
    renderer.drawText(SCRIPT_FONT_ID, metrics.contentSidePadding, metrics.topPadding + 4, tr(STR_NOW_READING));

    const bool hasRecentBook = !recentBooks.empty();
    const RecentBook* recentBook = hasRecentBook ? &recentBooks.front() : nullptr;
    const std::string displayTitle =
        hasRecentBook ? (recentBook->title.empty() ? filenameWithoutExtension(recentBook->path) : recentBook->title)
                      : std::string(tr(STR_NO_OPEN_BOOK));
    const int textWidth = pageWidth - HOME_CONTENT_MARGIN * 2;
    // One step down from the old 18 pt: the title stays the anchor of the
    // screen, but no longer competes with the header.
    const bool hasCover = !homeCoverPath.empty();
    const bool titleVisible = resolveHomeMetadataVisibility(SETTINGS.homeBookTitleMode, hasCover);
    const bool metadataInsidePlaceholder = hasRecentBook && !hasCover;
    const bool showTitle = titleVisible && !metadataInsidePlaceholder;
    const auto titleLines =
        showTitle ? renderer.wrappedText(UI_14_FONT_ID, displayTitle.c_str(), textWidth, 2, EpdFontFamily::BOLD)
                  : std::vector<std::string>{};
    const bool authorVisible = resolveHomeMetadataVisibility(SETTINGS.homeBookAuthorMode, hasCover) &&
                               (hasRecentBook ? !recentBook->author.empty() : true);
    const bool showAuthor = authorVisible && !metadataInsidePlaceholder;
    const char* authorLabel = hasRecentBook ? recentBook->author.c_str() : tr(STR_OPEN_LIBRARY_HINT);
    const int authorFontId = authorVisible ? homeAuthorFontId(renderer, authorLabel) : SCRIPT_SMALL_FONT_ID;
    const int authorLineHeight = showAuthor ? renderer.getLineHeight(authorFontId) : 0;
    const int titleLineHeight = renderer.getLineHeight(UI_14_FONT_ID);
    const HomeBookLayout layout =
        calculateHomeBookLayout(renderer, static_cast<int>(titleLines.size()), showTitle, showAuthor, authorLineHeight);

    for (size_t line = 0; line < titleLines.size(); ++line) {
      renderer.drawCenteredText(UI_14_FONT_ID, layout.titleTop + static_cast<int>(line) * titleLineHeight,
                                titleLines[line].c_str(), true, EpdFontFamily::BOLD);
    }

    bool coverDrawn = false;
    int coverVisualWidth = pageWidth - HOME_ACTION_SIDE_MARGIN * 2;
    if (!recentBooks.empty() && !homeCoverPath.empty()) {
      if (coverRegionCache && coverRegionCacheSize > 0 &&
          renderer.copyBufferToRegion(coverRegionX, coverRegionY, coverRegionWidth, coverRegionHeight,
                                      coverRegionCache.get(), coverRegionCacheSize)) {
        coverDrawn = true;
        coverVisualWidth = coverRegionWidth;
      } else {
        HalFile coverFile;
        if (Storage.openFileForRead("HOME", homeCoverPath, coverFile)) {
          Bitmap bitmap(coverFile);
          if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
            const float scale = std::min(static_cast<float>(layout.coverMaxWidth) / bitmap.getWidth(),
                                         static_cast<float>(layout.coverSlotHeight) / bitmap.getHeight());
            const int coverWidth = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
            const int coverHeight = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
            const int coverX = (pageWidth - coverWidth) / 2;
            const int coverY = layout.coverTop + (layout.coverSlotHeight - coverHeight) / 2;
            coverVisualWidth = coverWidth;
            const size_t regionSize = renderer.getRegionByteSize(coverX, coverY, coverWidth, coverHeight);
            char tileName[112];
            snprintf(tileName, sizeof(tileName), "%s/home_tile_v2_%d_%d_%d_%d_%d_%d_%d_%lu.bin", homeCachePath.c_str(),
                     static_cast<int>(renderer.getOrientation()), coverX, coverY, coverWidth, coverHeight,
                     bitmap.getWidth(), bitmap.getHeight(), static_cast<unsigned long>(coverFile.size()));

            // Across HomeActivity instances, restore the same prepared tile
            // from the book cache. A sequential ~10-20 KB read is far cheaper
            // than re-running the rotated per-pixel BMP scaler (~900 ms on X4).
            auto region = makeUniqueNoThrow<uint8_t[]>(regionSize);
            bool regionReady = false;
            if (region && regionSize > 0 && !homeCachePath.empty() && Storage.exists(tileName)) {
              HalFile tileFile;
              if (Storage.openFileForRead("HOME", tileName, tileFile) && tileFile.size() == regionSize &&
                  tileFile.read(region.get(), regionSize) == static_cast<int>(regionSize) &&
                  renderer.copyBufferToRegion(coverX, coverY, coverWidth, coverHeight, region.get(), regionSize)) {
                coverDrawn = true;
                regionReady = true;
              }
              tileFile.close();
            }

            if (!coverDrawn) {
              // Old EPUB caches can contain a decoder-native progressive JPEG
              // thumbnail (for example 141x225) even though its filename says
              // thumb_540.bmp. The generic bitmap path intentionally treats its
              // dimensions as maxima and never enlarges. Home has already
              // calculated the exact aspect-preserving destination, so opt in
              // to nearest-neighbour enlargement for 1-bit thumbnails here.
              if (bitmap.is1Bit()) {
                renderer.drawBitmap1Bit(bitmap, coverX, coverY, coverWidth, coverHeight, true);
              } else {
                renderer.drawBitmap(bitmap, coverX, coverY, coverWidth, coverHeight);
              }
              renderer.maskRoundedRectOutsideCorners(coverX, coverY, coverWidth, coverHeight, HOME_COVER_RADIUS,
                                                     Color::White);
              renderer.drawRoundedRect(coverX, coverY, coverWidth, coverHeight, 1, HOME_COVER_RADIUS, true);
              coverDrawn = true;

              if (region &&
                  renderer.copyRegionToBuffer(coverX, coverY, coverWidth, coverHeight, region.get(), regionSize)) {
                regionReady = true;
                if (!homeCachePath.empty()) {
                  if (Storage.exists(tileName)) Storage.remove(tileName);
                  HalFile tileFile;
                  if (Storage.openFileForWrite("HOME", tileName, tileFile)) {
                    tileFile.write(region.get(), regionSize);
                    tileFile.close();
                  }
                }
              }
            }

            // Keep the prepared tile in RAM while the Home carousel is alive;
            // neighbouring-page returns then need only a row memcpy.
            if (region && coverDrawn && regionReady) {
              coverRegionCache = std::move(region);
              coverRegionCacheSize = regionSize;
              coverRegionX = coverX;
              coverRegionY = coverY;
              coverRegionWidth = coverWidth;
              coverRegionHeight = coverHeight;
            }
          }
          coverFile.close();
        }
      }
    }

    if (!coverDrawn) {
      // A missing-cover book still owns the full artwork slot. Its metadata is
      // placed inside this deliberately quiet typographic cover instead of in
      // external rows that would make the placeholder visibly smaller than a
      // real cover. Explicit title/author visibility settings remain the final
      // authority; AUTO simply enables both fields when they exist.
      const int ghostHeight = std::min(layout.coverSlotHeight, layout.coverMaxWidth * 3 / 2);
      const int ghostWidth = ghostHeight * 2 / 3;
      const int ghostX = (pageWidth - ghostWidth) / 2;
      const int ghostY = layout.coverTop + (layout.coverSlotHeight - ghostHeight) / 2;
      coverVisualWidth = ghostWidth;
      renderer.drawRoundedRect(ghostX, ghostY, ghostWidth, ghostHeight, 1, HOME_COVER_RADIUS, true);
      renderer.drawIcon(LucideBookOpen32, pageWidth / 2 - 16, ghostY + ghostHeight / 2 - 16, 32, 32);

      if (metadataInsidePlaceholder) {
        const int placeholderTextWidth = std::max(1, ghostWidth - HOME_PLACEHOLDER_TEXT_MARGIN * 2);
        if (titleVisible) {
          const auto placeholderTitleLines =
              renderer.wrappedText(UI_14_FONT_ID, displayTitle.c_str(), placeholderTextWidth, 4, EpdFontFamily::BOLD);
          const int placeholderTitleLineHeight = renderer.getLineHeight(UI_14_FONT_ID);
          for (size_t line = 0; line < placeholderTitleLines.size(); ++line) {
            renderer.drawCenteredText(
                UI_14_FONT_ID,
                ghostY + HOME_PLACEHOLDER_TITLE_PADDING + static_cast<int>(line) * placeholderTitleLineHeight,
                placeholderTitleLines[line].c_str(), true, EpdFontFamily::BOLD);
          }
        }
        if (authorVisible) {
          const std::string placeholderAuthor = renderer.truncatedText(authorFontId, authorLabel, placeholderTextWidth);
          const int placeholderAuthorY =
              ghostY + ghostHeight - HOME_PLACEHOLDER_AUTHOR_PADDING - renderer.getLineHeight(authorFontId);
          renderer.drawCenteredText(authorFontId, placeholderAuthorY, placeholderAuthor.c_str());
        }
      }
    }

    if (showAuthor) {
      const std::string author = renderer.truncatedText(authorFontId, authorLabel, textWidth);
      renderer.drawCenteredText(authorFontId, layout.authorTop, author.c_str());
    }

    char readingTimeText[40];
    const uint32_t totalMinutes = readingSummary.readingSeconds / 60;
    const uint32_t hours = totalMinutes / 60;
    const uint32_t minutes = totalMinutes % 60;
    if (hours > 0) {
      snprintf(readingTimeText, sizeof(readingTimeText), tr(STR_HOME_HOURS_MINUTES_FORMAT),
               static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
    } else {
      snprintf(readingTimeText, sizeof(readingTimeText), tr(STR_HOME_MINUTES_FORMAT),
               static_cast<unsigned long>(totalMinutes));
    }

    // One data band instead of three near-title-sized rows: the bar carries
    // the shape, one small caption row under it carries the numbers —
    // progress on one side, invested time on the other. With no book yet the
    // band stays empty so the action button's position is unchanged.
    const int progressWidth = coverVisualWidth;
    const int progressX = (pageWidth - progressWidth) / 2;
    const int progressBarTop = layout.detailsTop;
    if (hasRecentBook) {
      renderer.fillRoundedRect(progressX, progressBarTop, progressWidth, HOME_PROGRESS_BAR_THICKNESS,
                               HOME_PROGRESS_BAR_THICKNESS / 2, Color::LightGray);
      const int progressFillWidth =
          std::clamp(progressWidth * static_cast<int>(readingSummary.progressPercent) / 100, 0, progressWidth);
      if (progressFillWidth >= HOME_PROGRESS_BAR_THICKNESS) {
        renderer.fillRoundedRect(progressX, progressBarTop, progressFillWidth, HOME_PROGRESS_BAR_THICKNESS,
                                 HOME_PROGRESS_BAR_THICKNESS / 2, Color::Black);
      }
    }

    const int captionTop = progressBarTop + HOME_PROGRESS_BAR_THICKNESS + HOME_PROGRESS_BAR_GAP;
    if (hasRecentBook) {
      // All the numbers in one quiet centred line under the bar. The
      // "Reading time:" label is dropped — after percent and pages, a
      // duration reads unambiguously as time spent.
      char caption[96] = "";
      if (readingSummary.totalPages > 0) {
        snprintf(caption, sizeof(caption), "%u%% · %lu / %lu · %s", readingSummary.progressPercent,
                 static_cast<unsigned long>(readingSummary.currentPage),
                 static_cast<unsigned long>(readingSummary.totalPages), readingTimeText);
      } else if (readingSummary.currentPage > 0) {
        snprintf(caption, sizeof(caption), "%u%% · %lu · %s", readingSummary.progressPercent,
                 static_cast<unsigned long>(readingSummary.currentPage), readingTimeText);
      } else {
        snprintf(caption, sizeof(caption), "%u%% · %s", readingSummary.progressPercent, readingTimeText);
      }
      const std::string captionText = renderer.truncatedText(SMALL_FONT_ID, caption, progressWidth);
      renderer.drawCenteredText(SMALL_FONT_ID, captionTop, captionText.c_str());
    }

  } else {
    // All three Home pages speak with the same handwritten voice — the hub
    // headers are part of "home", not navigation chrome. The actual Library
    // and Settings screens keep their structural headers.
    const char* header = pageIndex == 1 ? tr(STR_LIBRARY) : tr(STR_SETTINGS_TITLE);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, nullptr);
    renderer.drawText(SCRIPT_FONT_ID, metrics.contentSidePadding, metrics.topPadding + 4, header);

    constexpr std::array<UIIcon, 5> libraryIcons = {UIIcon::Book, UIIcon::Folder, UIIcon::Image, UIIcon::Favorite,
                                                    UIIcon::ReaderStats};
    const std::array<const char*, 5> libraryLabels = {tr(STR_BOOKS), tr(STR_FILES), tr(STR_GALLERY), tr(STR_FAVORITES),
                                                      tr(STR_READING_STATS)};
    constexpr std::array<UIIcon, 3> transferIcons = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot};
    const std::array<const char*, 3> transferLabels = {tr(STR_JOIN_NETWORK), tr(STR_CALIBRE_WIRELESS),
                                                       tr(STR_CREATE_HOTSPOT)};
    constexpr std::array<UIIcon, SettingsActivity::CATEGORY_COUNT> settingsIcons = {
        UIIcon::Interface, UIIcon::Power,       UIIcon::Reading, UIIcon::Controls,
        UIIcon::Files,     UIIcon::NetworkSync, UIIcon::System,
    };
    const std::array<const char*, SettingsActivity::CATEGORY_COUNT> settingsLabels = {
        tr(STR_SETTINGS_INTERFACE), tr(STR_SETTINGS_POWER),   tr(STR_SETTINGS_READING), tr(STR_SETTINGS_CONTROLS),
        tr(STR_SETTINGS_LIBRARY),   tr(STR_SETTINGS_NETWORK), tr(STR_SETTINGS_SYSTEM),
    };
    const int contentTop = metrics.topPadding + metrics.headerHeight + 12;
    if (pageIndex == 1) {
      // Use the same menu-row component as Settings so both Home hubs have
      // identical row height, spacing, typography, icon placement and
      // selection geometry. Transfer remains a non-selectable section label,
      // while the activity continues to expose eight actionable indices.
      constexpr int libraryItemCount = static_cast<int>(libraryIcons.size());
      constexpr int transferItemCount = static_cast<int>(transferIcons.size());
      const int menuPitch = metrics.menuRowHeight + metrics.menuSpacing;
      const int libraryMenuHeight = libraryItemCount * menuPitch - metrics.menuSpacing;
      const int transferSectionTop = contentTop + libraryMenuHeight;
      const int transferSectionHeight = metrics.listRowHeight;
      const int transferMenuTop = transferSectionTop + transferSectionHeight;

      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, libraryMenuHeight}, libraryItemCount,
          selectedIndex < libraryItemCount ? selectedIndex : -1,
          [&](const int index) { return std::string(libraryLabels[index]); },
          [&](const int index) { return libraryIcons[index]; });

      // Reuse the list section renderer for the branded handwritten label and
      // rule. With one row and no selection it is purely structural.
      GUI.drawList(
          renderer, Rect{0, transferSectionTop, pageWidth, transferSectionHeight}, 1, -1,
          [&](const int) { return std::string(tr(STR_TRANSFER_SECTION)); }, nullptr, nullptr, nullptr, false, nullptr,
          nullptr, [](const int) { return true; });

      GUI.drawButtonMenu(
          renderer, Rect{0, transferMenuTop, pageWidth, pageHeight - transferMenuTop - 96}, transferItemCount,
          selectedIndex >= libraryItemCount ? selectedIndex - libraryItemCount : -1,
          [&](const int index) { return std::string(transferLabels[index]); },
          [&](const int index) { return transferIcons[index]; });
    } else {
      GUI.drawButtonMenu(
          renderer, Rect{0, contentTop, pageWidth, pageHeight - contentTop - 96}, SettingsActivity::CATEGORY_COUNT,
          selectedIndex, [&](const int index) { return std::string(settingsLabels[index]); },
          [&](const int index) { return settingsIcons[index]; });
    }
  }

  GUI.drawPageDots(renderer, pageIndex, PAGE_COUNT);
  const auto labels = mappedInput.mapLabels("", tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
