#include <gtest/gtest.h>

#include "EpubProgressMath.h"

TEST(EpubProgressMath, CountsTheVisiblePageInsteadOfItsStartOffset) {
  const auto estimate = EpubProgressMath::estimatePages(0, 1000, 2000, 4, 10, false);
  EXPECT_EQ(estimate.current, 5u);
  EXPECT_EQ(estimate.total, 20u);
}

TEST(EpubProgressMath, LastReadablePageAlwaysMatchesTotal) {
  // Without the terminal clamp round(753 / 5.3) is 142 while ceil() is 143,
  // reproducing the real 752/753-style off-by-one seen on the Home screen.
  const auto estimate = EpubProgressMath::estimatePages(700, 53, 753, 9, 10, true);
  EXPECT_EQ(estimate.current, estimate.total);
  EXPECT_EQ(estimate.total, 143u);
}

TEST(EpubProgressMath, FirstVisiblePageIsOneBased) {
  const auto estimate = EpubProgressMath::estimatePages(0, 1000, 3000, 0, 10, false);
  EXPECT_EQ(estimate.current, 1u);
  EXPECT_EQ(estimate.total, 30u);
}

TEST(EpubProgressMath, RejectsIncompleteGeometry) {
  EXPECT_EQ(EpubProgressMath::estimatePages(0, 0, 1000, 0, 10, false).total, 0u);
  EXPECT_EQ(EpubProgressMath::estimatePages(0, 1000, 0, 0, 10, false).total, 0u);
  EXPECT_EQ(EpubProgressMath::estimatePages(0, 1000, 1000, 0, 0, false).total, 0u);
}
