#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace DictionaryWordNavigation {

namespace detail {

inline int64_t distance(const int64_t lhs, const int64_t rhs) { return lhs >= rhs ? lhs - rhs : rhs - lhs; }

template <typename Word>
int64_t centerX(const Word& word) {
  return static_cast<int64_t>(word.screenX) + std::max(1, word.screenW) / 2;
}

inline bool validLineStarts(const std::vector<size_t>& lineStarts, const size_t wordCount) {
  if (wordCount == 0 || lineStarts.empty() || lineStarts.front() != 0) return false;
  for (size_t i = 0; i < lineStarts.size(); ++i) {
    if (lineStarts[i] >= wordCount || (i > 0 && lineStarts[i] <= lineStarts[i - 1])) return false;
  }
  return true;
}

template <typename Word, typename IsSelectable>
size_t fallbackFocus(const std::vector<Word>& words, const IsSelectable& isSelectable) {
  if (words.empty()) return 0;
  const size_t middle = words.size() / 2;
  if (isSelectable(words[middle])) return middle;
  for (size_t distanceFromMiddle = 1; distanceFromMiddle < words.size(); ++distanceFromMiddle) {
    if (middle >= distanceFromMiddle && isSelectable(words[middle - distanceFromMiddle]))
      return middle - distanceFromMiddle;
    if (middle + distanceFromMiddle < words.size() && isSelectable(words[middle + distanceFromMiddle]))
      return middle + distanceFromMiddle;
  }
  return middle;
}

}  // namespace detail

// Selects a lookupable word near the visual centre of the actual text rather
// than the physical screen. This keeps short final pages balanced, works for
// both LTR and RTL coordinates, and performs no heap allocations. Invalid line
// metadata falls back to the middle word instead of risking an out-of-bounds
// cursor.
template <typename Word, typename IsSelectable>
size_t findCenteredFocus(const std::vector<Word>& words, const std::vector<size_t>& lineStarts,
                         IsSelectable&& isSelectable) {
  if (words.empty()) return 0;
  if (!detail::validLineStarts(lineStarts, words.size())) return detail::fallbackFocus(words, isSelectable);

  int64_t minX = std::numeric_limits<int64_t>::max();
  int64_t minY = std::numeric_limits<int64_t>::max();
  int64_t maxX = std::numeric_limits<int64_t>::min();
  int64_t maxY = std::numeric_limits<int64_t>::min();
  bool hasSelectableWord = false;
  for (const auto& word : words) {
    if (!isSelectable(word)) continue;
    hasSelectableWord = true;
    minX = std::min(minX, static_cast<int64_t>(word.screenX));
    minY = std::min(minY, static_cast<int64_t>(word.screenY));
    maxX = std::max(maxX, static_cast<int64_t>(word.screenX) + std::max(1, word.screenW));
    maxY = std::max(maxY, static_cast<int64_t>(word.screenY) + std::max(1, word.screenH));
  }
  if (!hasSelectableWord) return words.size() / 2;

  const int64_t targetX = minX + (maxX - minX) / 2;
  const int64_t targetY = minY + (maxY - minY) / 2;
  size_t bestLine = lineStarts.size();
  int64_t bestLineDistance = std::numeric_limits<int64_t>::max();

  for (size_t line = 0; line < lineStarts.size(); ++line) {
    const size_t begin = lineStarts[line];
    const size_t end = line + 1 < lineStarts.size() ? lineStarts[line + 1] : words.size();
    int64_t lineMinY = std::numeric_limits<int64_t>::max();
    int64_t lineMaxY = std::numeric_limits<int64_t>::min();
    bool lineHasSelectableWord = false;
    for (size_t wordIndex = begin; wordIndex < end; ++wordIndex) {
      const auto& word = words[wordIndex];
      if (!isSelectable(word)) continue;
      lineHasSelectableWord = true;
      lineMinY = std::min(lineMinY, static_cast<int64_t>(word.screenY));
      lineMaxY = std::max(lineMaxY, static_cast<int64_t>(word.screenY) + std::max(1, word.screenH));
    }
    if (!lineHasSelectableWord) continue;
    const int64_t lineCenterY = lineMinY + (lineMaxY - lineMinY) / 2;
    const int64_t lineDistance = detail::distance(lineCenterY, targetY);
    if (lineDistance < bestLineDistance) {
      bestLineDistance = lineDistance;
      bestLine = line;
    }
  }

  if (bestLine == lineStarts.size()) return detail::fallbackFocus(words, isSelectable);

  const size_t begin = lineStarts[bestLine];
  const size_t end = bestLine + 1 < lineStarts.size() ? lineStarts[bestLine + 1] : words.size();
  size_t bestWord = words.size();
  int64_t bestWordDistance = std::numeric_limits<int64_t>::max();
  for (size_t wordIndex = begin; wordIndex < end; ++wordIndex) {
    const auto& word = words[wordIndex];
    if (!isSelectable(word)) continue;
    const int64_t wordDistance = detail::distance(detail::centerX(word), targetX);
    if (wordDistance < bestWordDistance) {
      bestWordDistance = wordDistance;
      bestWord = wordIndex;
    }
  }
  return bestWord < words.size() ? bestWord : detail::fallbackFocus(words, isSelectable);
}

template <typename Word>
size_t findCenteredFocus(const std::vector<Word>& words, const std::vector<size_t>& lineStarts) {
  return findCenteredFocus(words, lineStarts, [](const Word&) { return true; });
}

}  // namespace DictionaryWordNavigation
