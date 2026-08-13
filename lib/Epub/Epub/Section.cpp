#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v28: recognize SVG <image href/xlink:href> while paginating (Calibre cover pages).
// Bumping the version rebuilds already-cached blank cover sections after OTA.
constexpr uint8_t SECTION_FILE_VERSION = 28;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering,
                                     const bool focusReadingEnabled) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(focusReadingEnabled) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, focusReadingEnabled);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool focusReadingEnabled) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }
  const uint64_t fileSize = file.fileSize64();
  if (fileSize < HEADER_SIZE) {
    file.close();
    clearCache();
    return false;
  }

  const auto corrupt = [this](const char* reason) {
    LOG_ERR("SCT", "Deserialization failed: %s", reason);
    file.close();
    clearCache();
    return false;
  };

  // Match parameters
  {
    uint8_t version = 0;
    if (!serialization::readPod(file, version)) return corrupt("truncated version");
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      LOG_ERR("SCT", "Unknown section cache version %u", version);
      return corrupt("version mismatch");
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    if (!serialization::readPod(file, fileFontId) || !serialization::readPod(file, fileLineCompression) ||
        !serialization::readPod(file, fileExtraParagraphSpacing) ||
        !serialization::readPod(file, fileParagraphAlignment) || !serialization::readPod(file, fileViewportWidth) ||
        !serialization::readPod(file, fileViewportHeight) || !serialization::readPod(file, fileHyphenationEnabled) ||
        !serialization::readPod(file, fileEmbeddedStyle) || !serialization::readPod(file, fileImageRendering) ||
        !serialization::readPod(file, fileFocusReadingEnabled))
      return corrupt("truncated settings header");

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering || focusReadingEnabled != fileFocusReadingEnabled) {
      return corrupt("parameters do not match");
    }
  }

  uint32_t pageLutOffset = 0;
  uint32_t anchorMapOffset = 0;
  uint32_t paragraphLutOffset = 0;
  uint32_t liLutOffset = 0;
  if (!serialization::readPod(file, pageCount) || !serialization::readPod(file, pageLutOffset) ||
      !serialization::readPod(file, anchorMapOffset) || !serialization::readPod(file, paragraphLutOffset) ||
      !serialization::readPod(file, liLutOffset))
    return corrupt("truncated layout header");
  if (pageLutOffset < HEADER_SIZE || pageLutOffset > anchorMapOffset || anchorMapOffset > paragraphLutOffset ||
      paragraphLutOffset > liLutOffset || liLutOffset > fileSize)
    return corrupt("invalid cache offsets");
  const uint64_t pageLutEnd =
      static_cast<uint64_t>(pageLutOffset) + static_cast<uint64_t>(pageCount) * sizeof(uint32_t);
  if (pageLutEnd > anchorMapOffset ||
      (pageCount > 0 && static_cast<uint64_t>(pageCount) * 4 > pageLutOffset - HEADER_SIZE))
    return corrupt("invalid page LUT");

  // Verify every page offset before accepting the cache. A torn LUT used to be
  // discovered only after navigating to the affected page, causing a repeatable
  // crash every time the book was reopened.
  for (uint16_t page = 0; page < pageCount; ++page) {
    if (!file.seek64(static_cast<uint64_t>(pageLutOffset) + page * sizeof(uint32_t)))
      return corrupt("cannot seek page LUT");
    uint32_t pageOffset = 0;
    if (!serialization::readPod(file, pageOffset) || pageOffset < HEADER_SIZE || pageOffset >= pageLutOffset)
      return corrupt("invalid page offset");
  }

  if (!file.seek64(paragraphLutOffset)) return corrupt("cannot seek paragraph LUT");
  uint16_t paragraphCount = 0;
  if (!serialization::readPod(file, paragraphCount) || paragraphCount != pageCount ||
      static_cast<uint64_t>(paragraphLutOffset) + sizeof(paragraphCount) +
              static_cast<uint64_t>(paragraphCount) * sizeof(uint16_t) >
          liLutOffset ||
      static_cast<uint64_t>(liLutOffset) + static_cast<uint64_t>(paragraphCount) * sizeof(uint16_t) > fileSize)
    return corrupt("invalid paragraph/list LUT");
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                const std::function<void(int percent)>& popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  [[maybe_unused]] uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    HalFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
  std::vector<PageLutEntry> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  const uint64_t fileSize = file.fileSize64();
  if (fileSize < HEADER_SIZE || currentPage < 0 || currentPage >= pageCount ||
      !file.seek64(HEADER_SIZE - sizeof(uint32_t) * 4)) {
    file.close();
    return nullptr;
  }
  uint32_t lutOffset = 0;
  if (!serialization::readPod(file, lutOffset) || lutOffset < HEADER_SIZE || lutOffset >= fileSize ||
      static_cast<uint64_t>(lutOffset) + static_cast<uint64_t>(pageCount) * sizeof(uint32_t) > fileSize ||
      !file.seek64(static_cast<uint64_t>(lutOffset) + sizeof(uint32_t) * static_cast<uint64_t>(currentPage))) {
    file.close();
    return nullptr;
  }
  uint32_t pagePos = 0;
  if (!serialization::readPod(file, pagePos) || pagePos < HEADER_SIZE || pagePos >= lutOffset ||
      !file.seek64(pagePos)) {
    file.close();
    return nullptr;
  }

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = this->loadPageFromSectionFile();
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& words = line.getBlock()->getWords();
          for (const auto& w : words) {
            if (!fullText.empty()) fullText += " ";
            fullText += w;
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint64_t fileSize = f.fileSize64();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  if (!f.seek64(0)) return std::nullopt;
  uint8_t version = 0;
  if (!serialization::readPod(f, version) || version != SECTION_FILE_VERSION ||
      !f.seek64(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t)))
    return std::nullopt;
  uint16_t count = 0;
  if (!serialization::readPod(f, count)) return std::nullopt;
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint64_t fileSize = f.fileSize64();
  if (fileSize < HEADER_SIZE || !f.seek64(HEADER_SIZE - sizeof(uint32_t) * 3)) return std::nullopt;
  uint32_t anchorMapOffset = 0;
  if (!serialization::readPod(f, anchorMapOffset)) return std::nullopt;
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek64(anchorMapOffset)) return std::nullopt;
  uint16_t count = 0;
  if (!serialization::readPod(f, count) || count > (fileSize - anchorMapOffset) / (sizeof(uint32_t) + sizeof(uint16_t)))
    return std::nullopt;
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::readString(f, key) || !serialization::readPod(f, page) || page >= pageCount)
      return std::nullopt;
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint64_t fileSize = f.fileSize64();
  if (fileSize < HEADER_SIZE || !f.seek64(HEADER_SIZE - sizeof(uint32_t) * 2)) return std::nullopt;
  uint32_t paragraphLutOffset = 0;
  if (!serialization::readPod(f, paragraphLutOffset)) return std::nullopt;
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek64(paragraphLutOffset)) return std::nullopt;
  uint16_t count = 0;
  if (!serialization::readPod(f, count)) return std::nullopt;
  if (count == 0) {
    return std::nullopt;
  }

  const uint64_t lutEnd =
      static_cast<uint64_t>(paragraphLutOffset) + sizeof(uint16_t) + static_cast<uint64_t>(count) * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    if (!serialization::readPod(f, pagePIdx)) return std::nullopt;
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint64_t fileSize = f.fileSize64();
  if (fileSize < HEADER_SIZE || !f.seek64(HEADER_SIZE - sizeof(uint32_t) * 2)) return std::nullopt;
  uint32_t paragraphLutOffset = 0;
  if (!serialization::readPod(f, paragraphLutOffset)) return std::nullopt;
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek64(paragraphLutOffset)) return std::nullopt;
  uint16_t count = 0;
  if (!serialization::readPod(f, count)) return std::nullopt;
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint64_t entryEnd =
      static_cast<uint64_t>(paragraphLutOffset) + sizeof(uint16_t) + static_cast<uint64_t>(page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek64(static_cast<uint64_t>(paragraphLutOffset) + sizeof(uint16_t) +
                static_cast<uint64_t>(page) * sizeof(uint16_t)))
    return std::nullopt;
  uint16_t pIdx = 0;
  if (!serialization::readPod(f, pIdx)) return std::nullopt;
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint64_t fileSize = f.fileSize64();
  if (fileSize < HEADER_SIZE || !f.seek64(HEADER_SIZE - sizeof(uint32_t))) return std::nullopt;
  uint32_t liLutOffset = 0;
  if (!serialization::readPod(f, liLutOffset)) return std::nullopt;
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!f.seek64(HEADER_SIZE - sizeof(uint32_t) * 2)) return std::nullopt;
  uint32_t paragraphLutOffset = 0;
  if (!serialization::readPod(f, paragraphLutOffset)) return std::nullopt;
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek64(paragraphLutOffset)) return std::nullopt;
  uint16_t count = 0;
  if (!serialization::readPod(f, count)) return std::nullopt;
  if (count == 0) {
    return std::nullopt;
  }

  const uint64_t lutEnd = static_cast<uint64_t>(liLutOffset) + static_cast<uint64_t>(count) * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek64(liLutOffset)) return std::nullopt;
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::readPod(f, pageLiIdx)) return std::nullopt;
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
