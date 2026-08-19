#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace EpubProgressMath {

struct PageEstimate {
  uint32_t current = 0;
  uint32_t total = 0;
};

inline PageEstimate estimatePages(const size_t previousBytes, const size_t chapterBytes, const size_t bookBytes,
                                  const int pageNumber, const int pageCount, const bool forceComplete) {
  if (chapterBytes == 0 || bookBytes == 0 || pageCount <= 0) return {};

  const double bytesPerPage = static_cast<double>(chapterBytes) / pageCount;
  const double totalEstimate = std::ceil(static_cast<double>(bookBytes) / bytesPerPage);
  const double visibleChapterProgress = std::clamp(static_cast<double>(pageNumber + 1) / pageCount, 0.0, 1.0);
  const double currentEstimate =
      std::round((static_cast<double>(previousBytes) + chapterBytes * visibleChapterProgress) / bytesPerPage);
  const double maximum = static_cast<double>(std::numeric_limits<uint32_t>::max());

  PageEstimate result;
  result.total = static_cast<uint32_t>(std::min(totalEstimate, maximum));
  result.current = forceComplete ? result.total : static_cast<uint32_t>(std::min(currentEstimate, maximum));
  if (result.total > 0) result.current = std::clamp<uint32_t>(result.current, 1, result.total);
  return result;
}

}  // namespace EpubProgressMath
