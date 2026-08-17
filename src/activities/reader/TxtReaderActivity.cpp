#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <Fb2Encoding.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderGesturesActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
// Bumped to 5 for encoding-aware line wrapping: page boundaries in a version-4
// cache were computed from mis-decoded single-byte text and no longer match.
constexpr uint8_t CACHE_VERSION = 6;  // v6: source fingerprint + strict/transactional records
constexpr uint32_t MAX_CACHED_PAGES = 8192;
constexpr uint8_t INDEX_SAVE_PAGE_INTERVAL = 8;
constexpr unsigned long INDEX_SAVE_TIME_INTERVAL_MS = 15000;
constexpr uint8_t PROGRESS_SAVE_PAGE_INTERVAL = 5;
constexpr unsigned long PROGRESS_SAVE_TIME_INTERVAL_MS = 30000;
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();
  readingStats.begin(txt->getCachePath());

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, txt->getTitle(), txt->getAuthor(), "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  if (initialized && txt && !pageOffsets.empty()) {
    maybeSavePageIndexCache(true);
    maybeSaveProgress(true);
  }
  if (txt) readingStats.finish(txt->getCachePath());

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::prepareEndOfBook() {
  if (!txt || endOfBookView.isPrepared()) return;
  const BookReadingStats snapshot = readingStats.completeAndSnapshot(txt->getCachePath());
  endOfBookView.prepare(txt->getPath(), txt->getTitle(), txt->getAuthor(), snapshot);
}

void TxtReaderActivity::loop() {
  if (atEndOfBook) {
    prepareEndOfBook();
    switch (endOfBookView.handleInput(mappedInput)) {
      case EndOfBookView::Action::Home:
        onGoHome();
        break;
      case EndOfBookView::Action::FileBrowser:
        activityManager.goToFileBrowser(txt ? txt->getPath() : "");
        break;
      case EndOfBookView::Action::OpenRecommendation:
        onSelectBook(endOfBookView.selectedPath());
        break;
      case EndOfBookView::Action::OpenLibrary:
        activityManager.goToLibrary();
        break;
      case EndOfBookView::Action::SelectionChanged:
        requestUpdate();
        break;
      case EndOfBookView::Action::None:
        break;
    }
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  // Confirm opens the gesture reference — the only readers with a menu are
  // EPUB/XTC, and without this a .txt reader had no way to discover the
  // controls at all.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startActivityForResult(makeUniqueNoThrow<ReaderGesturesActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  readingStats.pageTurn(nextTriggered);

  if (prevTriggered) {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  } else if (nextTriggered) {
    if (currentPage + 1 < static_cast<int>(pageOffsets.size())) {
      currentPage++;
      requestUpdate();
    } else if (fullyIndexed) {
      atEndOfBook = true;
      prepareEndOfBook();
      requestUpdate();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Load any pages discovered during earlier reading sessions. A new book is
  // deliberately not indexed in full here: large FB2/TXT files should open
  // immediately, with the next page offset discovered while rendering.
  const bool haveIndex = loadPageIndexCache();
  if (!haveIndex) {
    pageOffsets.clear();
    pageOffsets.push_back(0);
    totalPages = 1;
    fullyIndexed = false;
  }

  // Load saved progress
  loadProgress();

  // The index was rebuilt, so the saved page number no longer locates anything
  // and loadProgress() has just clamped it to the first page. Resume from the
  // saved byte offset instead: page numbering restarts (it is approximate for
  // plain text anyway, and shown with a leading "~"), but the reader opens where
  // the reader left off rather than at the top of the book.
  if (!haveIndex && savedByteOffset > 0 && savedByteOffset < txt->getFileSize()) {
    LOG_DBG("TRS", "Index rebuilt; resuming from saved offset %zu", savedByteOffset);
    pageOffsets.clear();
    pageOffsets.push_back(savedByteOffset);
    currentPage = 0;
    totalPages = 1;
    fullyIndexed = false;
  }

  initialized = true;
}

namespace {
// Number of code points in the first `byteLength` bytes of a UTF-8 string. Used
// to translate a position in transcoded text back to a source-byte count.
size_t countCodepoints(const std::string& text, const size_t byteLength) {
  size_t count = 0;
  const size_t limit = std::min(byteLength, text.size());
  for (size_t i = 0; i < limit; i++) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) count++;
  }
  return count;
}
}  // namespace

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();
  const bool transcoding = Fb2Encoding::isSupported(txt->getEncoding());

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    // Prewarm from the decoded text: the raw bytes of a single-byte encoding are
    // different code points, so priming with them would load the wrong glyphs and
    // leave the wrap loop thrashing anyway.
    if (transcoding) {
      const std::string decoded =
          Fb2Encoding::toUtf8(txt->getEncoding(), reinterpret_cast<const char*>(buffer), chunkSize);
      renderer.ensureSdCardFontReady(cachedFontId, decoded.c_str(), /*styleMask=*/0x01);
    } else {
      renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
    }
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF), converting legacy
    // single-byte text to UTF-8 so the glyph renderer receives real code points.
    // A byte-order mark only ever appears at the very start of the file.
    const char* lineStart = reinterpret_cast<char*>(buffer + pos);
    size_t lineDisplayLen = displayLen;
    if (offset + pos == 0 && txt->getContentStart() > 0 && lineDisplayLen >= txt->getContentStart()) {
      lineStart += txt->getContentStart();
      lineDisplayLen -= txt->getContentStart();
    }
    std::string line = transcoding ? Fb2Encoding::toUtf8(txt->getEncoding(), lineStart, lineDisplayLen)
                                   : std::string(lineStart, lineDisplayLen);

    // Track position within this source line (in bytes from pos). When
    // transcoding, one source byte is exactly one code point, so source bytes are
    // counted as code points in the converted string rather than as its bytes.
    size_t lineBytePos = (lineStart - reinterpret_cast<char*>(buffer + pos));

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      // Build a visual line word by word. The previous implementation started
      // with the entire (up to 8 KB) paragraph and repeatedly measured shorter
      // prefixes. FB2 paragraphs are commonly one very long XML line, making
      // that algorithm quadratic and causing multi-minute indexing.
      const int spaceWidth = renderer.getTextAdvanceX(cachedFontId, " ", EpdFontFamily::REGULAR);
      size_t breakPos = 0;
      size_t cursor = 0;
      int usedWidth = 0;

      while (cursor < line.length()) {
        while (cursor < line.length() && line[cursor] == ' ') cursor++;
        const size_t wordStart = cursor;
        while (cursor < line.length() && line[cursor] != ' ') cursor++;
        const size_t wordEnd = cursor;
        if (wordStart == wordEnd) break;

        const std::string word = line.substr(wordStart, wordEnd - wordStart);
        const int wordWidth = renderer.getTextAdvanceX(cachedFontId, word.c_str(), EpdFontFamily::REGULAR);
        const int separatorWidth = breakPos > 0 ? spaceWidth : 0;
        if (usedWidth + separatorWidth + wordWidth <= viewportWidth) {
          usedWidth += separatorWidth + wordWidth;
          breakPos = wordEnd;
          continue;
        }

        if (breakPos > 0) break;

        // A single word is wider than the viewport. Find the largest UTF-8
        // prefix that fits with logarithmic rather than byte-by-byte probing.
        size_t low = 1;
        size_t high = word.length();
        size_t best = 0;
        while (low <= high) {
          const size_t midpoint = low + (high - low) / 2;
          size_t candidate = midpoint;
          while (candidate > 0 && candidate < word.length() &&
                 (static_cast<unsigned char>(word[candidate]) & 0xC0) == 0x80) {
            candidate--;
          }
          if (candidate == 0) {
            low = midpoint + 1;
            continue;
          }
          const std::string prefix = word.substr(0, candidate);
          if (renderer.getTextAdvanceX(cachedFontId, prefix.c_str(), EpdFontFamily::REGULAR) <= viewportWidth) {
            best = candidate;
            low = midpoint + 1;
          } else {
            if (candidate <= 1) break;
            high = candidate - 1;
          }
        }
        if (best == 0) {
          best = 1;
          while (best < word.length() && (static_cast<unsigned char>(word[best]) & 0xC0) == 0x80) best++;
        }
        breakPos = wordStart + best;
        break;
      }

      if (breakPos == 0) breakPos = line.length();

      outLines.push_back(line.substr(0, breakPos));

      // Skip whitespace at the break point.
      size_t skipChars = breakPos;
      while (skipChars < line.length() && line[skipChars] == ' ') skipChars++;
      lineBytePos += transcoding ? countCodepoints(line, skipChars) : skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    GUI.drawReaderMessage(renderer, tr(STR_EMPTY_FILE), /*script=*/true);
    renderer.displayBuffer();
    return;
  }

  if (atEndOfBook) {
    prepareEndOfBook();
    endOfBookView.render(renderer, mappedInput);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= static_cast<int>(pageOffsets.size())) currentPage = pageOffsets.size() - 1;

  // Load current page content. loadPageAtOffset has early-return paths that
  // never touch nextOffset, so seed it and honour the result: an SD read hiccup
  // used to push an uninitialised value into the persisted page index, which
  // then reopened the book at a garbage offset.
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset = offset;
  currentPageLines.clear();
  const bool pageLoaded = loadPageAtOffset(offset, currentPageLines, nextOffset);
  currentPageEndOffset = nextOffset;
  if (!pageLoaded) {
    renderer.clearScreen();
    renderPage();
    return;
  }

  bool indexChanged = false;
  if (nextOffset > offset && currentPage + 1 == static_cast<int>(pageOffsets.size())) {
    if (nextOffset < txt->getFileSize()) {
      pageOffsets.push_back(nextOffset);
    } else {
      fullyIndexed = true;
    }
    indexChanged = true;
  }

  if (fullyIndexed) {
    totalPages = pageOffsets.size();
  } else if (nextOffset > 0) {
    const uint64_t estimate =
        (static_cast<uint64_t>(txt->getFileSize()) * static_cast<uint64_t>(currentPage + 1) + nextOffset - 1) /
        nextOffset;
    totalPages = std::max<int>(pageOffsets.size(), static_cast<int>(estimate));
  } else {
    totalPages = pageOffsets.size();
  }

  if (indexChanged) {
    indexDirty = true;
    if (pagesSinceIndexSave < UINT8_MAX) ++pagesSinceIndexSave;
    maybeSavePageIndexCache();
  }

  renderer.clearScreen();
  renderPage();
  readingStats.pageShown();

  // Save progress
  maybeSaveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  if (SETTINGS.readerInvertColors) {
    renderer.invertScreen();
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = txt->getFileSize() > 0 ? currentPageEndOffset * 100.0f / txt->getFileSize() : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void TxtReaderActivity::saveProgress() const {
  // Bytes 0-1 hold the page number, as they always have; bytes 4-7 add the
  // current page's byte offset. The page number alone is meaningless once the
  // page index is rebuilt (any font, margin or alignment change does that), and
  // the reader used to be clamped back to page 1 as a result. The offset survives
  // re-indexing because it addresses the source file, not the layout.
  uint8_t data[8] = {};
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  const uint32_t offset = (currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size()))
                              ? static_cast<uint32_t>(pageOffsets[currentPage])
                              : 0;
  data[4] = offset & 0xFF;
  data[5] = (offset >> 8) & 0xFF;
  data[6] = (offset >> 16) & 0xFF;
  data[7] = (offset >> 24) & 0xFF;
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: page %d", currentPage);
  }
}

void TxtReaderActivity::maybeSaveProgress(const bool force) {
  if (!txt || pageOffsets.empty()) return;
  const int distance =
      lastSavedProgressPage < 0 ? PROGRESS_SAVE_PAGE_INTERVAL : std::abs(currentPage - lastSavedProgressPage);
  const unsigned long now = millis();
  if (!force && distance < PROGRESS_SAVE_PAGE_INTERVAL && now - lastProgressSaveMs < PROGRESS_SAVE_TIME_INTERVAL_MS)
    return;
  saveProgress();
  lastSavedProgressPage = currentPage;
  lastProgressSaveMs = now;
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8] = {};
    const int read = f.read(data, sizeof(data));
    if (read >= 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      // Files written before the offset was added are 4 bytes long.
      savedByteOffset = read >= 8 ? static_cast<size_t>(data[4]) | (static_cast<size_t>(data[5]) << 8) |
                                        (static_cast<size_t>(data[6]) << 16) | (static_cast<size_t>(data[7]) << 24)
                                  : 0;
      LOG_DBG("TRS", "Loaded progress: page %d/%d, offset %zu", currentPage, totalPages, savedByteOffset);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint8_t: whether the final page has been discovered
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic = 0;
  if (!serialization::readPod(f, magic)) return false;
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version = 0;
  if (!serialization::readPod(f, version)) return false;
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint64_t fileSize = 0;
  uint64_t sourceFingerprint = 0;
  if (!serialization::readPod(f, fileSize) || !serialization::readPod(f, sourceFingerprint)) return false;
  if (fileSize != txt->getFileSize() || sourceFingerprint != txt->getSourceFingerprint()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth = 0;
  if (!serialization::readPod(f, cachedWidth)) return false;
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines = 0;
  if (!serialization::readPod(f, cachedLines)) return false;
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId = 0;
  if (!serialization::readPod(f, fontId)) return false;
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin = 0;
  if (!serialization::readPod(f, margin)) return false;
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment = 0;
  if (!serialization::readPod(f, alignment)) return false;
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint8_t complete = 0;
  if (!serialization::readPod(f, complete) || complete > 1) return false;
  fullyIndexed = complete != 0;

  uint32_t numPages = 0;
  if (!serialization::readPod(f, numPages)) return false;

  // numPages comes straight off the card. A page cannot be shorter than one
  // byte, so the file size is a hard ceiling; without it a corrupt header
  // (0xFFFFFFFF) asks for a multi-gigabyte reserve and aborts the firmware.
  if (numPages == 0 || numPages > MAX_CACHED_PAGES || numPages > txt->getFileSize() + 1) {
    LOG_DBG("TRS", "Cache page count %u implausible, rebuilding", static_cast<unsigned>(numPages));
    return false;
  }

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  uint32_t previousOffset = 0;
  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset = 0;
    if (!serialization::readPod(f, offset)) {
      LOG_DBG("TRS", "Cache truncated at page %u, rebuilding", static_cast<unsigned>(i));
      pageOffsets.clear();
      return false;
    }
    if (offset >= txt->getFileSize() || (i > 0 && offset <= previousOffset)) {
      LOG_DBG("TRS", "Cache contains invalid/non-increasing page offset");
      pageOffsets.clear();
      return false;
    }
    pageOffsets.push_back(offset);
    previousOffset = offset;
  }

  if (pageOffsets.empty()) {
    return false;
  }
  totalPages = pageOffsets.size();
  indexDirty = false;
  pagesSinceIndexSave = 0;
  lastIndexSaveMs = millis();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

bool TxtReaderActivity::savePageIndexCache() {
  if (!txt || pageOffsets.empty() || pageOffsets.size() > MAX_CACHED_PAGES || txt->getFileSize() > UINT32_MAX)
    return false;
  std::string cachePath = txt->getCachePath() + "/index.bin";
  const std::string tmpPath = cachePath + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", tmpPath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return false;
  }

  // Write header using serialization module
  bool ok = serialization::writePod(f, CACHE_MAGIC) && serialization::writePod(f, CACHE_VERSION) &&
            serialization::writePod(f, static_cast<uint64_t>(txt->getFileSize())) &&
            serialization::writePod(f, txt->getSourceFingerprint()) &&
            serialization::writePod(f, static_cast<int32_t>(viewportWidth)) &&
            serialization::writePod(f, static_cast<int32_t>(linesPerPage)) &&
            serialization::writePod(f, static_cast<int32_t>(cachedFontId)) &&
            serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin)) &&
            serialization::writePod(f, cachedParagraphAlignment) &&
            serialization::writePod(f, static_cast<uint8_t>(fullyIndexed ? 1 : 0)) &&
            serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    ok = ok && serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  f.flush();
  f.close();
  if (!ok) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.replaceFileFromTemp(cachePath.c_str(), tmpPath.c_str())) {
    LOG_ERR("TRS", "Failed to install page index cache");
    return false;
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
  indexDirty = false;
  pagesSinceIndexSave = 0;
  lastIndexSaveMs = millis();
  return true;
}

void TxtReaderActivity::maybeSavePageIndexCache(const bool force) {
  if (!indexDirty) return;
  const unsigned long now = millis();
  if (!force && pagesSinceIndexSave < INDEX_SAVE_PAGE_INTERVAL && now - lastIndexSaveMs < INDEX_SAVE_TIME_INTERVAL_MS)
    return;
  savePageIndexCache();
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent =
      txt && txt->getFileSize() > 0 ? static_cast<int>(currentPageEndOffset * 100.0f / txt->getFileSize() + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
