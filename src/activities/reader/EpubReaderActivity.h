#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "BookReadingStats.h"
#include "BookmarkEntry.h"
#include "EndOfBookView.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  unsigned long pageShownAtMs = 0UL;
  uint32_t sessionReadingSeconds = 0UL;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  int lastSavedSpineIndex = -1;
  uint8_t pagesSinceProgressSave = 0;
  unsigned long lastProgressSaveMs = 0;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool isPdfDocument = false;
  uint8_t pdfZoomOption = 0;
  int pdfViewportIndex = 0;
  int pdfViewportCount = 1;
  // Retained so the reader menu can open showing the rate that is actually
  // running. The menu returns its picker value on cancel too, so without this
  // every visit to the menu handed back a fresh 0 and silently stopped
  // automatic page turning.
  uint8_t currentPageTurnOption = 0;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  EndOfBookView endOfBookView;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // A corrupt section page gets one automatic rebuild. Persistent parser/SD
  // failures must stop with an error instead of scheduling an endless render →
  // delete → rebuild loop that drains the battery and wears the card.
  int cacheRecoverySpine = -1;
  int cacheRecoveryPage = -1;
  uint8_t cacheRecoveryAttempts = 0;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  // Matches FavoriteBooksStore's cap. Each bookmark stores a text summary, so an
  // unbounded list grows the JSON file and the session heap without limit.
  static constexpr size_t MAX_BOOKMARKS_PER_BOOK = 64;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openDictionary();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void applyPdfZoom(uint8_t option);
  void loadPdfZoom();
  void savePdfZoom() const;
  bool movePdfViewport(bool forward);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  float getCurrentBookProgressPercent() const;
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown");
  bool estimateRemainingPages(float& remainingPages) const;
  bool estimateTimeLeftSeconds(uint32_t& seconds) const;
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();
  void prepareEndOfBook();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              int initialRefreshCountdown)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  // Automatic page turning is hands-free reading, so there are no button events
  // to keep the inactivity timer alive. Without this the device deep-sleeps
  // mid-session while it is still turning pages.
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
