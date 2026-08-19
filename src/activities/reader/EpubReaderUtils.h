#pragma once

#include <Epub.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "EpubProgressMath.h"
#include "ProgressFile.h"

namespace EpubReaderUtils {

inline void writeLe32(uint8_t* dst, const uint32_t value) {
  dst[0] = value & 0xFF;
  dst[1] = (value >> 8) & 0xFF;
  dst[2] = (value >> 16) & 0xFF;
  dst[3] = (value >> 24) & 0xFF;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  // Byte 6 is a denormalized whole-book percentage for lightweight library
  // rendering. Bytes 7..14 contain estimated whole-book page counters for the
  // Home screen. Existing readers intentionally read only the first six bytes,
  // so this remains backward-compatible with every progress.bin consumer.
  uint8_t data[15]{};
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const int spineCount = epub.getSpineItemsCount();
  const bool finished = spineCount > 0 && spineIndex >= spineCount;
  const float chapterProgress = pageCount > 0 ? static_cast<float>(pageNumber) / pageCount : 0.0f;
  const float bookProgress =
      finished ? 1.0f : epub.calculateProgress(spineIndex, std::clamp(chapterProgress, 0.0f, 1.0f));
  data[6] = static_cast<uint8_t>(std::clamp(static_cast<int>(bookProgress * 100.0f + 0.5f), 0, 100));

  uint32_t currentBookPage = 0;
  uint32_t totalBookPages = 0;
  if (pageCount > 0 && spineCount > 0) {
    const int estimateSpine = finished ? spineCount - 1 : std::clamp(spineIndex, 0, spineCount - 1);
    const size_t previousBytes = estimateSpine > 0 ? epub.getCumulativeSpineItemSize(estimateSpine - 1) : 0;
    const size_t cumulativeBytes = epub.getCumulativeSpineItemSize(estimateSpine);
    const size_t chapterBytes = cumulativeBytes > previousBytes ? cumulativeBytes - previousBytes : 0;
    const size_t bookBytes = epub.getBookSize();
    const bool lastReadablePage = estimateSpine == spineCount - 1 && pageNumber >= pageCount - 1;
    const auto estimate = EpubProgressMath::estimatePages(previousBytes, chapterBytes, bookBytes, pageNumber, pageCount,
                                                          finished || lastReadablePage);
    currentBookPage = estimate.current;
    totalBookPages = estimate.total;
  }
  writeLe32(data + 7, currentBookPage);
  writeLe32(data + 11, totalBookPages);
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, sizeof(data))) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
