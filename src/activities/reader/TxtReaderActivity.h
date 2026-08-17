#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "EndOfBookView.h"
#include "ReadingSessionStats.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  // Byte offset of the page that was open when progress was last saved. Used to
  // resume in the right place after the page index has been rebuilt.
  size_t savedByteOffset = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;
  bool fullyIndexed = false;
  // True while the end-of-book screen is shown: forward from the last page
  // lands here first instead of silently ejecting the user to Home.
  bool atEndOfBook = false;
  size_t currentPageEndOffset = 0;
  bool indexDirty = false;
  uint8_t pagesSinceIndexSave = 0;
  unsigned long lastIndexSaveMs = 0;
  int lastSavedProgressPage = -1;
  unsigned long lastProgressSaveMs = 0;
  ReadingSessionStats readingStats;
  EndOfBookView endOfBookView;

  void prepareEndOfBook();

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  bool loadPageIndexCache();
  bool savePageIndexCache();
  void maybeSavePageIndexCache(bool force = false);
  void saveProgress() const;
  void maybeSaveProgress(bool force = false);
  void loadProgress();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
