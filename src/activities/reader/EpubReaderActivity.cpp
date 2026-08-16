#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "BookInfoActivity.h"
#include "BookStatsActivity.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubDictionaryActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "FavoriteBooksStore.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "PdfViewport.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderGesturesActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "achievements/AchievementSystem.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;
constexpr unsigned long MIN_READING_STATS_PAGE_MS = 2000UL;
constexpr uint32_t MIN_SESSION_SECONDS_FOR_COUNT = 60UL;
constexpr uint32_t MIN_SESSION_SECONDS_FOR_TIME = 10UL;
constexpr uint32_t IDLE_READING_THRESHOLD_SECONDS = 120UL;
constexpr uint16_t MIN_PACE_SAMPLE_SECONDS = 2U;
constexpr char PDF_VIEW_SETTINGS_FILE[] = "/pdf_view.bin";
constexpr uint8_t PDF_VIEW_SETTINGS_VERSION = 1;

uint32_t addSaturatedUint32(const uint32_t lhs, const uint32_t rhs) {
  return (UINT32_MAX - lhs < rhs) ? UINT32_MAX : lhs + rhs;
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/read" (excludes NUL)
  if (path.size() <= n || path[n] != '/') return false;
  // FAT is case-insensitive, so a book opened via "/Read/..." lives in the same
  // directory as one opened via "/read/...". A case-sensitive compare treated the
  // former as not-yet-moved and produced a " (2)" duplicate on the card.
  for (size_t i = 0; i < n; i++) {
    const char expected = READ_FOLDER[i];
    char actual = path[i];
    if (actual >= 'A' && actual <= 'Z') actual = static_cast<char>(actual - 'A' + 'a');
    if (actual != expected) return false;
  }
  return true;
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();
  isPdfDocument = FsHelpers::hasPdfExtension(epub->getPath());
  loadPdfZoom();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }
  stats = BookReadingStats::load(epub->getCachePath());
  globalStats = GlobalReadingStats::load();
  sessionReadingSeconds = 0;
  pageShownAtMs = 0;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  recordCurrentPageReadingTime("reader_exit");
  const bool hasEnoughTimeForEstimate = sessionReadingSeconds >= MIN_SESSION_SECONDS_FOR_TIME;

  if (sessionReadingSeconds >= MIN_SESSION_SECONDS_FOR_TIME) {
    if (sessionReadingSeconds >= MIN_SESSION_SECONDS_FOR_COUNT) {
      stats.sessionCount++;
      globalStats.totalSessions++;
    }
    stats.totalReadingSeconds = addSaturatedUint32(stats.totalReadingSeconds, sessionReadingSeconds);
    globalStats.totalReadingSeconds = addSaturatedUint32(globalStats.totalReadingSeconds, sessionReadingSeconds);
    if (hasSessionStartLocalDateTime) {
      stats.recordReadingSpan(sessionStartLocalDateTime, sessionReadingSeconds);
      globalStats.recordReadingSpan(sessionStartLocalDateTime, sessionReadingSeconds);
    }
    if (!stats.startDateManual && !stats.startDate.isValid() && hasSessionStartLocalDateTime &&
        sessionReadingSeconds >= 120) {
      stats.startDate = sessionStartLocalDateTime.date;
    }
  }

  if (stats.isCompleted) {
    stats.estimatedTimeLeftSeconds = 0;
  } else {
    uint32_t est = 0;
    stats.estimatedTimeLeftSeconds = (hasEnoughTimeForEstimate && estimateTimeLeftSeconds(est)) ? est : 0;
  }

  if (epub) {
    stats.save(epub->getCachePath());
  }
  globalStats.save();
  ACHIEVEMENTS.refresh(globalStats);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Flush the coalesced reading position; the per-render save now skips most
  // page turns, so exit is the write of record.
  if (epub && section) {
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  }

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      const int currentPage = section ? section->currentPage + 1 : 0;
      const int totalPages = section ? section->pageCount : 0;
      float bookProgress = 0.0f;
      if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      recordCurrentPageReadingTime("reader_menu");
      startActivityForResult(
          makeUniqueNoThrow<EpubReaderMenuActivity>(
              renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
              SETTINGS.orientation, currentPageTurnOption, !currentPageFootnotes.empty(), !cachedBookmarks.empty(),
              FAVORITE_BOOKS.contains(epub->getPath()), isPdfDocument, pdfZoomOption),
          [this](const ActivityResult& result) {
            // Always apply orientation change even if the menu was cancelled
            const auto& menu = std::get<MenuResult>(result.data);
            applyOrientation(menu.orientation);
            toggleAutoPageTurn(menu.pageTurnOption);
            applyPdfZoom(menu.pdfZoomOption);
            if (!result.isCancelled) {
              onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
            }
          });
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          ignoreNextConfirmRelease = true;
          openDictionary();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    // "Quick-return from footnotes" (pwrBtnFootnoteBack) decides this branch.
    // The toggle was offered in Controls, saved, and then read by nothing: the
    // power button returned from a footnote whether the user wanted it to or
    // not. With it off the power button keeps opening footnotes — including
    // ones inside a footnote — and BACK is the way out, which it already is.
    if (footnoteDepth > 0 && SETTINGS.pwrBtnFootnoteBack) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            makeUniqueNoThrow<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      if (isPdfDocument && pdfZoomOption > 0) pdfViewportIndex = std::numeric_limits<int>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (isPdfDocument && pdfZoomOption > 0 && movePdfViewport(nextTriggered)) {
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
        RenderLock lock(*this);
        currentSpineIndex = sync.spineIndex;
        nextPageNumber = sync.page;
        section.reset();
      }
    }
  };

  switch (action) {
    // Orientation and auto-turn are not actions: the menu cycles their values
    // in place and returns them alongside the action, and the handler above
    // applies both before we get here. Named so the compiler can keep checking
    // this switch for genuinely unhandled entries.
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
    case EpubReaderMenuActivity::MenuAction::PDF_ZOOM:
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PAGE: {
      if (!section || section->pageCount == 0) break;
      const int initialPage = section->currentPage + 1;
      const int maximumPage = section->pageCount;
      startActivityForResult(
          makeUniqueNoThrow<IntervalSelectionActivity>(renderer, mappedInput, "PageSelection", StrId::STR_GO_TO_PAGE,
                                                       StrId::STR_PAGE_STEP_HINT, initialPage, 1, maximumPage, 1, 10,
                                                       StrId::STR_PAGE_VALUE_FORMAT, true),
          [this](const ActivityResult& result) {
            if (result.isCancelled || !section) return;
            const auto selected = static_cast<int>(std::get<IntervalResult>(result.data).value);
            section->currentPage = std::clamp(selected - 1, 0, section->pageCount - 1);
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          makeUniqueNoThrow<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionary();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(
          makeUniqueNoThrow<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& footnoteResult = std::get<FootnoteResult>(result.data);
              navigateToHref(footnoteResult.href, true);
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          makeUniqueNoThrow<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      // The item sits one wrap-press from the menu's default selection and
      // deleting costs a full re-index, so it must not fire unconfirmed.
      startActivityForResult(makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_CACHE),
                                                                     epub ? epub->getTitle() : std::string{}),
                             [this](const ActivityResult& confirmResult) {
                               if (confirmResult.isCancelled) {
                                 requestUpdate();
                                 return;
                               }
                               {
                                 RenderLock lock(*this);
                                 if (epub && section) {
                                   uint16_t backupSpine = currentSpineIndex;
                                   uint16_t backupPage = section->currentPage;
                                   uint16_t backupPageCount = section->pageCount;
                                   section.reset();
                                   epub->clearCache();
                                   epub->setupCacheDir();
                                   if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
                                     LOG_ERR("ERS", "Failed to save progress before cache clear");
                                   }
                                 }
                               }
                               onGoHome();
                             });
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (!launchKOReaderSync()) {
        // Without credentials the launch is refused — say so instead of
        // closing the menu and doing nothing.
        GUI.drawPopup(renderer, tr(STR_KOREADER_SETUP_HINT));
        renderer.displayBuffer();
        delay(1200);
        requestUpdate();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          makeUniqueNoThrow<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      const float currentProgress = getCurrentBookProgressPercent();

      // Include the current page time before opening the stats screen so:
      //  - totalSeconds reflects time spent on this page
      //  - forward-page pace has a valid dwell sample when estimate is needed
      uint32_t sessionTimeSeconds = 0;
      if (currentPageReadingSecondsForStats(sessionTimeSeconds, "reader_stats")) {
        sessionReadingSeconds = addSaturatedUint32(sessionReadingSeconds, sessionTimeSeconds);
      }

      uint32_t liveEstimatedTimeLeftSeconds = 0;
      const bool hasSessionEstimate = estimateTimeLeftSeconds(liveEstimatedTimeLeftSeconds) && !stats.isCompleted;
      const bool showAllDeviceStats = GlobalReadingStats::hasSyncedStats();
      if (showAllDeviceStats) {
        startActivityForResult(
            makeUniqueNoThrow<BookStatsActivity>(
                renderer, mappedInput, epub ? epub->getTitle() : std::string{tr(STR_UNNAMED)},
                epub ? epub->getCachePath() : std::string{}, stats, currentProgress, hasSessionEstimate,
                liveEstimatedTimeLeftSeconds, globalStats, GlobalReadingStats::loadAggregated(globalStats), false),
            [this](const ActivityResult& result) {
              if (!result.isCancelled && epub) {
                const auto* maybeUpdated = std::get_if<ReadingStatsResult>(&result.data);
                if (maybeUpdated && maybeUpdated->updated) {
                  stats = BookReadingStats::load(epub->getCachePath());
                  globalStats = GlobalReadingStats::load();
                }
              }
            });
      } else {
        startActivityForResult(makeUniqueNoThrow<BookStatsActivity>(
                                   renderer, mappedInput, epub ? epub->getTitle() : std::string{tr(STR_UNNAMED)},
                                   epub ? epub->getCachePath() : std::string{}, stats, currentProgress,
                                   hasSessionEstimate, liveEstimatedTimeLeftSeconds, globalStats, false),
                               [this](const ActivityResult& result) {
                                 if (!result.isCancelled && epub) {
                                   const auto* maybeUpdated = std::get_if<ReadingStatsResult>(&result.data);
                                   if (maybeUpdated && maybeUpdated->updated) {
                                     stats = BookReadingStats::load(epub->getCachePath());
                                     globalStats = GlobalReadingStats::load();
                                   }
                                 }
                               });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GESTURES:
      startActivityForResult(makeUniqueNoThrow<ReaderGesturesActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    case EpubReaderMenuActivity::MenuAction::READING_SETTINGS: {
      startActivityForResult(makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, 2, true),
                             [this](const ActivityResult&) {
                               RenderLock lock(*this);
                               if (section) {
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->pageCount;
                                 nextPageNumber = section->currentPage;
                                 section.reset();
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOK_INFO: {
      const int page = section ? section->currentPage + 1 : 0;
      const int pages = section ? section->pageCount : 0;
      startActivityForResult(
          makeUniqueNoThrow<BookInfoActivity>(renderer, mappedInput, epub->getTitle(), epub->getAuthor(),
                                              epub->getLanguage(), epub->getPath(), page, pages,
                                              clampPercent(static_cast<int>(getCurrentBookProgressPercent() + 0.5f))),
          [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::OPEN_FROM_FILE: {
      startActivityForResult(makeUniqueNoThrow<FileBrowserActivity>(renderer, mappedInput, epub->getPath(),
                                                                    FileBrowserActivity::Mode::PickBook),
                             [](const ActivityResult& result) {
                               if (result.isCancelled || !std::holds_alternative<FilePathResult>(result.data)) return;
                               activityManager.goToReader(std::get<FilePathResult>(result.data).path);
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_FAVORITE: {
      FAVORITE_BOOKS.toggle(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      requestUpdate();
      break;
    }
  }
}

void EpubReaderActivity::openDictionary() {
  if (!section || section->pageCount == 0) {
    requestUpdate();
    return;
  }
  auto page = section->loadPageFromSectionFile();
  if (!page) {
    GUI.drawPopup(renderer, tr(STR_PAGE_LOAD_ERROR));
    renderer.displayBuffer();
    delay(900);
    requestUpdate();
    return;
  }
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  startActivityForResult(makeUniqueNoThrow<EpubDictionaryActivity>(renderer, mappedInput, std::move(page),
                                                                   SETTINGS.getReaderFontId(), marginLeft, marginTop),
                         [this](const ActivityResult&) { requestUpdate(); });
}

float EpubReaderActivity::getCurrentBookProgressPercent() const {
  if (!epub) {
    return -1.0f;
  }

  if (section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }

  if (cachedChapterTotalPageCount > 0) {
    const float chapterProgress = static_cast<float>(nextPageNumber) / static_cast<float>(cachedChapterTotalPageCount);
    return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }

  if (currentSpineIndex > 0) {
    return epub->calculateProgress(currentSpineIndex, 0.0f) * 100.0f;
  }

  return 0.0f;
}

bool EpubReaderActivity::currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const {
  if (pageShownAtMs == 0UL) {
    return false;
  }

  const unsigned long now = millis();
  const unsigned long elapsedMs = now - pageShownAtMs;
  if (elapsedMs < MIN_READING_STATS_PAGE_MS) {
    LOG_DBG("ERS", "Skipping stats sample (%s): dwell %lu ms", source ? source : "unknown", elapsedMs);
    return false;
  }

  seconds = static_cast<uint32_t>(elapsedMs / 1000UL);
  if (seconds == 0) {
    return false;
  }

  if (!epub) {
    return false;
  }

  return true;
}

void EpubReaderActivity::recordCurrentPageReadingTime(const char* source) {
  uint32_t seconds = 0;
  if (!currentPageReadingSecondsForStats(seconds, source)) {
    return;
  }

  sessionReadingSeconds = addSaturatedUint32(sessionReadingSeconds, seconds);
}

bool EpubReaderActivity::estimateRemainingPages(float& remainingPages) const {
  const float progressPercent = getCurrentBookProgressPercent();
  if (progressPercent <= 0.0f || progressPercent >= 100.0f || stats.totalPagesTurned == 0) {
    return false;
  }

  remainingPages = static_cast<float>(stats.totalPagesTurned) * (100.0f - progressPercent) / progressPercent;
  if (remainingPages < 0.0f || remainingPages > 1e9f) {
    return false;
  }

  return true;
}

bool EpubReaderActivity::estimateTimeLeftSeconds(uint32_t& seconds) const {
  float remainingPages = 0.0f;
  if (!estimateRemainingPages(remainingPages) || stats.avgSecondsPerForwardPage == 0) {
    return false;
  }

  const uint64_t totalSeconds =
      (static_cast<uint64_t>(stats.avgSecondsPerForwardPage) * static_cast<uint64_t>(remainingPages + 0.5f));
  if (totalSeconds == 0 || totalSeconds > std::numeric_limits<uint32_t>::max()) {
    return false;
  }

  seconds = static_cast<uint32_t>(totalSeconds);
  return true;
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(makeUniqueNoThrow<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
    pdfViewportIndex = 0;
    pdfViewportCount = 1;
  }
}

void EpubReaderActivity::loadPdfZoom() {
  pdfZoomOption = 0;
  pdfViewportIndex = 0;
  pdfViewportCount = 1;
  if (!isPdfDocument || !epub) return;

  HalFile file;
  if (!Storage.openFileForRead("PDFVIEW", epub->getCachePath() + PDF_VIEW_SETTINGS_FILE, file)) return;
  uint8_t version = 0;
  uint8_t option = 0;
  if (file.read(&version, sizeof(version)) == sizeof(version) && file.read(&option, sizeof(option)) == sizeof(option) &&
      version == PDF_VIEW_SETTINGS_VERSION) {
    pdfZoomOption = PdfViewport::clampOption(option);
  }
}

void EpubReaderActivity::savePdfZoom() const {
  if (!isPdfDocument || !epub) return;
  HalFile file;
  if (!Storage.openFileForWrite("PDFVIEW", epub->getCachePath() + PDF_VIEW_SETTINGS_FILE, file)) {
    LOG_ERR("ERS", "Failed to save PDF zoom setting");
    return;
  }
  const uint8_t version = PDF_VIEW_SETTINGS_VERSION;
  file.write(&version, sizeof(version));
  file.write(&pdfZoomOption, sizeof(pdfZoomOption));
  file.close();
}

void EpubReaderActivity::applyPdfZoom(const uint8_t option) {
  if (!isPdfDocument) return;
  const uint8_t clamped = PdfViewport::clampOption(option);
  if (pdfZoomOption == clamped) return;
  pdfZoomOption = clamped;
  pdfViewportIndex = 0;
  pdfViewportCount = 1;
  pagesUntilFullRefresh = 0;
  savePdfZoom();
  requestUpdate();
}

bool EpubReaderActivity::movePdfViewport(const bool forward) {
  if (!section || pdfZoomOption == 0) return false;
  if (forward) {
    if (pdfViewportIndex + 1 < pdfViewportCount) {
      recordCurrentPageReadingTime("pdf_viewport");
      ++pdfViewportIndex;
      requestUpdate();
    } else {
      pdfViewportIndex = 0;
      pageTurn(true);
    }
  } else {
    if (pdfViewportIndex > 0) {
      recordCurrentPageReadingTime("pdf_viewport");
      --pdfViewportIndex;
      requestUpdate();
    } else {
      if (currentSpineIndex == 0 && section->currentPage == 0) return true;
      // The previous page may have a different image aspect ratio. A large
      // sentinel is clamped to that page's final tile during render.
      pdfViewportIndex = std::numeric_limits<int>::max();
      pageTurn(false);
    }
  }
  return true;
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    currentPageTurnOption = 0;
    return;
  }

  currentPageTurnOption = selectedPageTurnOption;
  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  uint32_t currentPageSeconds = 0;
  if (currentPageReadingSecondsForStats(currentPageSeconds, "page_turn")) {
    sessionReadingSeconds = addSaturatedUint32(sessionReadingSeconds, currentPageSeconds);
    if (isForwardTurn) {
      stats.recordForwardPageRead(currentPageSeconds);
      stats.totalPagesTurned = addSaturatedUint32(stats.totalPagesTurned, 1);
      globalStats.totalPagesTurned = addSaturatedUint32(globalStats.totalPagesTurned, 1);
    }
  }

  if (isForwardTurn) {
    if (isPdfDocument && pdfZoomOption > 0) pdfViewportIndex = 0;
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (isPdfDocument && pdfZoomOption > 0 && pdfViewportIndex == 0) {
      pdfViewportIndex = std::numeric_limits<int>::max();
    }
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    GUI.drawReaderMessage(renderer, tr(STR_END_OF_BOOK), /*script=*/true);
    // Not a reading page, so the no-legend rule does not apply — and one of
    // these buttons silently leaves the book, which deserves a warning.
    const auto labels = mappedInput.mapLabels("", "", tr(STR_BACK), tr(STR_HOME));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    pageShownAtMs = millis();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    // Plain new aborts under -fno-exceptions, so a tight heap would panic the
    // device instead of failing back to the library like the other error paths.
    section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
    if (!section) {
      LOG_ERR("ERS", "Out of memory loading section %d", currentSpineIndex);
      // Leaving the previous page on the panel made a failed load read as a
      // dead button — say what happened instead.
      renderer.clearScreen();
      GUI.drawReaderMessage(renderer, tr(STR_MEMORY_ERROR));
      renderer.displayBuffer();
      showPendingSyncSaveError();
      return;
    }

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      LOG_DBG("ERS", "Cache not found, building...");

      const Rect indexingPopup = GUI.drawPopup(renderer, tr(STR_INDEXING));

      const auto popupFn = [this, indexingPopup](const int percent) {
        GUI.fillPopupProgress(renderer, indexingPopup, percent);
      };

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        renderer.clearScreen();
        GUI.drawReaderMessage(renderer, tr(STR_ERROR_GENERAL_FAILURE));
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    GUI.drawReaderMessage(renderer, tr(STR_EMPTY_CHAPTER), /*script=*/true);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    pageShownAtMs = millis();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    GUI.drawReaderMessage(renderer, tr(STR_OUT_OF_BOUNDS));
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    pageShownAtMs = millis();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      const int failedPage = section->currentPage;
      const bool samePage = cacheRecoverySpine == currentSpineIndex && cacheRecoveryPage == failedPage;
      if (!samePage) {
        cacheRecoverySpine = currentSpineIndex;
        cacheRecoveryPage = failedPage;
        cacheRecoveryAttempts = 0;
      }
      if (cacheRecoveryAttempts == 0) {
        cacheRecoveryAttempts = 1;
        LOG_ERR("ERS", "Failed to load page from SD - rebuilding section cache once");
        section->clearCache();
        section.reset();
        requestUpdate();
      } else {
        LOG_ERR("ERS", "Section cache rebuild failed again; stopping automatic retries");
        renderer.clearScreen();
        GUI.drawReaderMessage(renderer, tr(STR_PAGE_LOAD_ERROR));
        renderStatusBar();
        renderer.displayBuffer();
      }
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    cacheRecoverySpine = -1;
    cacheRecoveryPage = -1;
    cacheRecoveryAttempts = 0;

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    [[maybe_unused]] const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  // Coalesced: this used to persist on every page render — four FAT directory
  // operations per turn (~1,200 for a 300-page book) to store 6 bytes. A
  // chapter change saves immediately; within a chapter, every 5th turn or 30 s
  // of dwell. onExit() flushes the final position, so an abrupt power cut
  // loses a few page turns of position at most, never data.
  const bool chapterChanged = currentSpineIndex != lastSavedSpineIndex;
  pagesSinceProgressSave++;
  if (chapterChanged || pagesSinceProgressSave >= 5 || millis() - lastProgressSaveMs >= 30000) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
      lastSavedSpineIndex = currentSpineIndex;
      pagesSinceProgressSave = 0;
      lastProgressSaveMs = millis();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
  pageShownAtMs = millis();
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  [[maybe_unused]] const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  if (isPdfDocument && pdfZoomOption > 0) {
    int16_t imageX = 0;
    int16_t imageY = 0;
    int16_t imageWidth = 0;
    int16_t imageHeight = 0;
    if (page->getSingleImageGeometry(imageX, imageY, imageWidth, imageHeight)) {
      const PdfViewportRect viewport = PdfViewport::calculate(imageWidth, imageHeight, pdfZoomOption, pdfViewportIndex);
      pdfViewportIndex = viewport.index;
      pdfViewportCount = viewport.count();
      if (page->renderSingleImageViewport(renderer, orientedMarginLeft, orientedMarginTop, viewport.x, viewport.y,
                                          viewport.width, viewport.height, imageWidth, imageHeight)) {
        renderStatusBar();
        // PDF raster pages are already dithered in their pixel cache. A second
        // grayscale reconstruction adds many SD reads but no extra musical
        // detail, so zoomed tiles use the reader's normal fast/clean cadence.
        ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
        LOG_DBG("ERS", "Rendered PDF viewport %d/%d at %u%% in %lums", pdfViewportIndex + 1, pdfViewportCount,
                PdfViewport::zoomPercent(pdfZoomOption), millis() - t0);
        return;
      }
    }
    // Text/reflow PDF pages and malformed image pages keep the regular EPUB
    // renderer even when a zoom preference is saved for the document.
    pdfViewportIndex = 0;
    pdfViewportCount = 1;
  }

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  [[maybe_unused]] const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  [[maybe_unused]] const auto tBwRender = millis();

  if (SETTINGS.readerInvertColors) {
    // Night mode is intentionally monochrome on X4. Skipping the grayscale
    // passes avoids a second 8 KB scratch allocation and keeps controller RAM
    // in sync with the inverted framebuffer used for differential refresh.
    renderer.invertScreen();
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    [[maybe_unused]] const auto tEnd = millis();
    LOG_DBG("ERS", "Page render (inverted): prewarm=%lums render=%lums display=%lums total=%lums", tPrewarm - t0,
            tBwRender - tPrewarm, tEnd - tBwRender, tEnd - t0);
    return;
  }

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  [[maybe_unused]] const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      [[maybe_unused]] const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      [[maybe_unused]] const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      [[maybe_unused]] const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      [[maybe_unused]] const auto tCleanup = millis();

      [[maybe_unused]] const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        [[maybe_unused]] const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      [[maybe_unused]] const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      [[maybe_unused]] const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      [[maybe_unused]] const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      [[maybe_unused]] const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      [[maybe_unused]] const auto tBwRestore = millis();

      [[maybe_unused]] const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      [[maybe_unused]] const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. The label is 1-based ("5 / 20"), but the
  // fraction must use the 0-based page like the reader menu, Book Info and
  // Reading Stats do — mixing the two made the status bar read one page ahead of
  // every other surface.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(section->currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (isPdfDocument && pdfZoomOption > 0 && pdfViewportCount > 1) {
    char viewportLabel[40];
    snprintf(viewportLabel, sizeof(viewportLabel), "PDF %u%%  %d/%d", PdfViewport::zoomPercent(pdfZoomOption),
             pdfViewportIndex + 1, pdfViewportCount);
    title = viewportLabel;
  } else if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  bool pushedPosition = false;
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    pushedPosition = true;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    // Only undo a push that actually happened. The push is skipped at max depth
    // or without a loaded section, and popping anyway discarded a real saved
    // position, so Back landed on the wrong page.
    if (pushedPosition) footnoteDepth--;
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->pageCount;
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    // Bounded like FAVORITE_BOOKS. Each entry carries a text summary, so an
    // unbounded list grows both the JSON file and the heap for the whole session;
    // the newest are kept because the list is ordered most-recent-first.
    if (cachedBookmarks.size() > MAX_BOOKMARKS_PER_BOOK) {
      cachedBookmarks.resize(MAX_BOOKMARKS_PER_BOOK);
    }
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  } else if (!bookmarkRemoved) {
    ACHIEVEMENTS.record(AchievementEvent::BookmarkAdded);
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const ProgressRange pageRange =
      getPageProgressRange(epub, currentSpineIndex, section->currentPage, section->pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, section->pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
