#pragma once

namespace LayoutGeometry {

struct VerticalSpan {
  int y;
  int height;
};

// Content between a screen header and the bottom hardware-button legend.
// Metrics already set buttonHintsHeight to zero when hints are disabled.
constexpr VerticalSpan menuContentSpan(const int screenHeight, const int topPadding, const int headerHeight,
                                       const int verticalSpacing, const int buttonHintsHeight) {
  const int top = topPadding + headerHeight + verticalSpacing;
  const int rawBottom = screenHeight - buttonHintsHeight - verticalSpacing;
  const int bottom = rawBottom < top ? top : rawBottom;
  return VerticalSpan{top, bottom - top};
}

constexpr int centeredBlockTop(const int areaY, const int areaHeight, const int blockHeight) {
  if (areaHeight <= blockHeight) return areaY;
  return areaY + (areaHeight - blockHeight) / 2;
}

}  // namespace LayoutGeometry
