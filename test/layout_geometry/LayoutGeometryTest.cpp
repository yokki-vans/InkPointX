#include <gtest/gtest.h>

#include "components/LayoutGeometry.h"

TEST(LayoutGeometry, X4MenuContentExcludesHeaderAndButtonLegend) {
  const auto content = LayoutGeometry::menuContentSpan(800, 8, 68, 8, 44);
  EXPECT_EQ(content.y, 84);
  EXPECT_EQ(content.height, 664);
}

TEST(LayoutGeometry, X3MenuContentUsesItsActualPanelHeight) {
  const auto content = LayoutGeometry::menuContentSpan(792, 8, 68, 8, 44);
  EXPECT_EQ(content.y, 84);
  EXPECT_EQ(content.height, 656);
}

TEST(LayoutGeometry, CentresSingleAndMultilineBlocksInContent) {
  EXPECT_EQ(LayoutGeometry::centeredBlockTop(84, 664, 28), 402);
  EXPECT_EQ(LayoutGeometry::centeredBlockTop(84, 664, 84), 374);
}

TEST(LayoutGeometry, NeverPlacesOversizedBlockBeforeContent) {
  EXPECT_EQ(LayoutGeometry::centeredBlockTop(84, 40, 80), 84);
}
