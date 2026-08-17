#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "RecentBooksStore.h"

class GfxRenderer;
class MappedInputManager;

// Shared, lightweight completion experience for every reader format.  It keeps
// only the three rows that can be displayed so reaching the final page does not
// trigger a library rescan or retain the full favourites catalog in RAM.
class EndOfBookView {
  static constexpr size_t MAX_RECOMMENDATIONS = 3;

  std::vector<RecentBook> recommendations;
  std::string finishedTitle;
  std::string finishedAuthor;
  uint32_t readingSeconds = 0;
  size_t selectedIndex = 0;
  bool prepared = false;

  void addCandidate(const RecentBook& book, const std::string& currentPath);

 public:
  enum class Action : uint8_t { None, Home, FileBrowser, OpenRecommendation, OpenLibrary, SelectionChanged };

  void prepare(const std::string& currentPath, const std::string& title, const std::string& author,
               const BookReadingStats& stats);
  Action handleInput(MappedInputManager& mappedInput);
  bool hasRecommendation() const { return !recommendations.empty(); }
  const std::string& selectedPath() const;
  bool isPrepared() const { return prepared; }
  void render(GfxRenderer& renderer, MappedInputManager& mappedInput) const;
};
