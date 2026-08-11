#include <gtest/gtest.h>

#include <vector>

#include "DictionaryWordNavigation.h"

namespace {
struct WordBox {
  int screenX = 0;
  int screenY = 0;
  int screenW = 10;
  int screenH = 10;
  bool selectable = true;
};

size_t select(const std::vector<WordBox>& words, const std::vector<size_t>& lines) {
  return DictionaryWordNavigation::findCenteredFocus(words, lines, [](const WordBox& word) { return word.selectable; });
}
}  // namespace

TEST(DictionaryWordNavigation, EmptyAndSingleWordPagesAreSafe) {
  EXPECT_EQ(select({}, {}), 0u);
  EXPECT_EQ(select({{42, 120, 30, 18, true}}, {0}), 0u);
}

TEST(DictionaryWordNavigation, SelectsMiddleWordOfMiddleTextLine) {
  const std::vector<WordBox> words = {
      {20, 100}, {120, 100}, {220, 100}, {20, 140}, {120, 140}, {220, 140}, {20, 180}, {120, 180}, {220, 180},
  };
  EXPECT_EQ(select(words, {0, 3, 6}), 4u);
}

TEST(DictionaryWordNavigation, UsesGeometryInsteadOfGlobalWordMedian) {
  const std::vector<WordBox> words = {
      {20, 100}, {80, 100}, {140, 100}, {200, 100}, {260, 100}, {130, 140, 40, 16}, {20, 180}, {260, 180},
  };
  EXPECT_EQ(select(words, {0, 5, 6}), 5u);
}

TEST(DictionaryWordNavigation, CentresWithinShortPageContentNotPhysicalScreen) {
  const std::vector<WordBox> words = {
      {100, 40},
      {180, 40},
      {100, 70},
      {180, 70},
  };
  EXPECT_EQ(select(words, {0, 2}), 0u);
}

TEST(DictionaryWordNavigation, RtlCoordinatesStillChooseVisualCentre) {
  const std::vector<WordBox> words = {
      {310, 100}, {210, 100}, {110, 100}, {310, 140}, {210, 140}, {110, 140}, {310, 180}, {210, 180}, {110, 180},
  };
  EXPECT_EQ(select(words, {0, 3, 6}), 4u);
}

TEST(DictionaryWordNavigation, SkipsPunctuationOnlyCandidate) {
  const std::vector<WordBox> words = {
      {80, 120, 30, 16, true},
      {140, 120, 30, 16, false},
      {200, 120, 30, 16, true},
  };
  EXPECT_EQ(select(words, {0}), 0u);
}

TEST(DictionaryWordNavigation, InvalidLineMetadataFallsBackToNearestSelectableMiddleWord) {
  const std::vector<WordBox> words = {
      {20, 100, 20, 16, true},
      {60, 100, 20, 16, true},
      {100, 100, 20, 16, false},
      {140, 100, 20, 16, true},
  };
  EXPECT_EQ(select(words, {0, 2, 2}), 1u);
}

TEST(DictionaryWordNavigation, EvenLineTieIsDeterministicAndTopBiased) {
  const std::vector<WordBox> words = {
      {100, 100, 30, 16, true},
      {100, 140, 30, 16, true},
      {100, 180, 30, 16, true},
      {100, 220, 30, 16, true},
  };
  EXPECT_EQ(select(words, {0, 1, 2, 3}), 1u);
}
