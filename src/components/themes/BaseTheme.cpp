#include "BaseTheme.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/bookmark.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconWidth = 16;
constexpr int bookmarkStatusIconHeight = 14;
constexpr int bookmarkStatusIconGap = 4;
constexpr int bookmarkStatusIconTopCrop = 2;
constexpr int buttonHintSideMargin = 20;
constexpr int buttonHintGroupGap = 12;
constexpr int buttonHintHeight = 34;
// Raised from 7: on some units the panel sits a couple of millimetres lower in
// the bezel and the hint bar looked like it was sliding off the bottom edge.
// The pills are the one element pinned to that edge, so they are what shows it.
constexpr int buttonHintBottomMargin = 14;
constexpr int buttonHintCornerRadius = buttonHintHeight / 2;
constexpr int selectionCornerRadius = 12;

Rect buttonHintGroupRect(const GfxRenderer& renderer, const int groupIndex) {
  const int pageWidth = renderer.getScreenWidth();
  const int groupWidth = (pageWidth - 2 * buttonHintSideMargin - buttonHintGroupGap) / 2;
  return Rect{buttonHintSideMargin + groupIndex * (groupWidth + buttonHintGroupGap),
              renderer.getScreenHeight() - buttonHintBottomMargin - buttonHintHeight, groupWidth, buttonHintHeight};
}

bool pointInsideRoundedRect(const int x, const int y, const Rect& rect, const int radius) {
  const int nearestX = std::clamp(x, rect.x + radius, rect.x + rect.width - radius - 1);
  const int nearestY = std::clamp(y, rect.y + radius, rect.y + rect.height - radius - 1);
  const int dx = x - nearestX;
  const int dy = y - nearestY;
  return dx * dx + dy * dy <= radius * radius;
}

// A deterministic 1/16-density surface reads as a very light gray on the X4
// without creating a large charged area that can linger after a fast refresh.
void fillSparseRoundedRect(const GfxRenderer& renderer, const Rect& rect, const int radius) {
  if (rect.width <= 4 || rect.height <= 4) return;
  for (int y = rect.y + 2; y < rect.y + rect.height - 2; y += 4) {
    const int rowOffset = ((y - rect.y) / 4) % 2 == 0 ? 0 : 2;
    for (int x = rect.x + 2 + rowOffset; x < rect.x + rect.width - 2; x += 4) {
      if (pointInsideRoundedRect(x, y, rect, radius)) renderer.drawPixel(x, y, true);
    }
  }
}

bool statusBarTextLaneVisible() {
  return SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
         SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
         (SETTINGS.showBatteryIndicator && SETTINGS.statusBarBattery) ||
         (SETTINGS.statusBarClock && halClock.hasValidTime());
}

void drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int bytesPerRow = bookmarkStatusIconWidth / 8;
  for (int row = 0; row < bookmarkStatusIconHeight; ++row) {
    for (int col = 0; col < bookmarkStatusIconWidth; ++col) {
      const uint8_t byte = BookmarkStatusIcon[(row + bookmarkStatusIconTopCrop) * bytesPerRow + col / 8];
      const uint8_t mask = 1U << (7 - (col % 8));
      renderer.drawPixel(x + col, y + row, (byte & mask) != 0);
    }
  }
}

}  // namespace

Rect BaseTheme::batteryBodyRect(const Rect box, const int centerY) {
  // A battery reads as a battery at its proportions, not at its outline. The
  // old icon was 13x12 with a radius-3 corner — square, heavily rounded, and
  // with a 1 px gap before its terminal, so it read as a rounded box with a
  // speck next to it. Three quarters of the box height puts the body near the
  // 3:2 of the real thing, and the terminal is drawn flush against it.
  // Five sixths of the box: ten pixels against the twelve-pixel digits beside
  // it, which is the ratio a status bar wants — the icon reads as the same size
  // as the number without matching it stroke for stroke. Three quarters left it
  // looking dainty next to the text.
  const int bodyHeight = std::clamp(box.height * 5 / 6, 7, box.height);
  const int bodyWidth = box.width - batteryTerminalWidth;
  return Rect{box.x, centerY - bodyHeight / 2, bodyWidth, bodyHeight};
}

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, const Rect body) {
  if (body.width < 7 || body.height < 6) return;
  // Radius 1: at nine pixels tall anything more rounds the corners into the
  // 2 px wall and the fill inside stops looking level.
  renderer.drawRoundedRect(body.x, body.y, body.width, body.height, 1, 1, true);
  const int terminalHeight = std::clamp(body.height / 3, 3, 5);
  renderer.fillRoundedRect(body.x + body.width, body.y + (body.height - terminalHeight) / 2, batteryTerminalWidth,
                           terminalHeight, 1, Color::Black);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Knocked out white on the black fill. Five rows, four columns: the body's
  // interior is five pixels tall now, and the previous eight-row bolt was
  // drawn past the fill into the outline.
  renderer.drawLine(boltX + 2, boltY + 0, boltX + 3, boltY + 0, false);
  renderer.drawLine(boltX + 1, boltY + 1, boltX + 2, boltY + 1, false);
  renderer.drawLine(boltX + 0, boltY + 2, boltX + 3, boltY + 2, false);
  renderer.drawLine(boltX + 1, boltY + 3, boltX + 2, boltY + 3, false);
  renderer.drawLine(boltX + 0, boltY + 4, boltX + 1, boltY + 4, false);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  // rect is the body, terminal excluded: inset two pixels on every side.
  const int maxFillWidth = rect.width - 4;
  const int fillHeight = rect.height - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  // +1 to round up so we always fill at least one pixel
  int filledWidth = percentage * maxFillWidth / 100;
  if (percentage > 0 && filledWidth == 0) filledWidth = 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 6;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  if (filledWidth > 0) {
    renderer.fillRoundedRect(rect.x + 2, rect.y + 2, filledWidth, fillHeight, 1, Color::Black);
  }

  if (charging) {
    drawBatteryLightningBolt(renderer, rect.x + 3, rect.y + 2);
  }
}

int BaseTheme::batteryDigitsCenterY(const GfxRenderer& renderer, const Rect rect, const int fontId,
                                    const char* percentageText) {
  // Align the icon to the digits themselves. The old code offset it by two
  // fifths of the line height (and by a hardcoded ten pixels in the header),
  // which is the font's box, not its marks: change the size and the icon
  // drifts off centre. Ask the font where the ink of these digits sits.
  int inkTop = 0, inkHeight = 0;
  if (percentageText && renderer.getTextInkBounds(fontId, percentageText, &inkTop, &inkHeight)) {
    return rect.y + inkTop + inkHeight / 2;
  }
  // Nothing to align to (percentage hidden): hold the same optical position by
  // centring on the text line the digits would have occupied.
  const int lineHeight = renderer.getLineHeight(fontId);
  return rect.y + (lineHeight > 0 ? lineHeight / 2 : rect.height / 2);
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage,
                                const int fontId) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const auto percentageText = std::to_string(percentage) + "%";

  if (showPercentage) {
    renderer.drawText(fontId, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str());
  }

  const int centerY = batteryDigitsCenterY(renderer, rect, fontId, showPercentage ? percentageText.c_str() : nullptr);
  const Rect body = batteryBodyRect(rect, centerY);
  drawBatteryOutline(renderer, body);
  fillBatteryIcon(renderer, body, percentage);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const auto percentageText = std::to_string(percentage) + "%";

  if (showPercentage) {
    const int textWidth = renderer.getTextWidth(batteryPercentFontId, percentageText.c_str());
    renderer.drawText(batteryPercentFontId, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
  }

  const int centerY =
      batteryDigitsCenterY(renderer, rect, batteryPercentFontId, showPercentage ? percentageText.c_str() : nullptr);
  const Rect body = batteryBodyRect(rect, centerY);
  drawBatteryOutline(renderer, body);
  fillBatteryIcon(renderer, body, percentage);
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  const int barHeight = std::min(rect.height, std::max(6, UITheme::getInstance().getMetrics().progressBarHeight));
  const int barY = rect.y + (rect.height - barHeight) / 2;
  renderer.fillRoundedRect(rect.x, barY, rect.width, barHeight, barHeight / 2, Color::LightGray);
  const int fillWidth = rect.width * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRoundedRect(rect.x, barY, fillWidth, barHeight, barHeight / 2, Color::Black);
  }

  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y + rect.height + 10, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (!SETTINGS.showButtonHints) return;
  UITheme::getInstance().markButtonHintsVisible();

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const char* labels[] = {btn1, btn2, btn3, btn4};

  std::string hintText;
  for (const char* label : labels) {
    if (label && *label) hintText.append(label).push_back('\n');
  }
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->warmGlyphCache(MICRO_FONT_ID, hintText.c_str(), 1U << EpdFontFamily::REGULAR);
  }

  // The X4 has two long physical rockers, each split into two independently
  // clickable sections. Reproduce that silhouette on-screen so the label-to-
  // hardware mapping is readable at a glance.
  // Always the compact caption size: adapting the size to the labels made the
  // bar change between two sizes from screen to screen, which read as an
  // accident. A label slightly wider than its half still borrows room from
  // its neighbour by shifting the group's seam (each side keeps at least 30%
  // so the two-button shape stays readable).
  constexpr int hintFontId = MICRO_FONT_ID;
  const auto labelWidth = [&](const char* label) {
    return (label && label[0] != '\0') ? renderer.getTextWidth(hintFontId, label, EpdFontFamily::REGULAR) + 6 : 0;
  };

  bool anyPressed = false;
  for (int groupIndex = 0; groupIndex < 2; ++groupIndex) {
    const Rect group = buttonHintGroupRect(renderer, groupIndex);
    renderer.fillRect(group.x, group.y, group.width, group.height, false);
    const int minSection = group.width * 3 / 10;
    const int need0 = labelWidth(labels[groupIndex * 2]);
    const int need1 = labelWidth(labels[groupIndex * 2 + 1]);
    int seam = group.width / 2;
    if ((need0 > seam || need1 > group.width - seam) && need0 + need1 <= group.width) {
      seam = std::clamp(need0 > seam ? need0 : group.width - need1, minSection, group.width - minSection);
    }

    for (int sectionIndex = 0; sectionIndex < 2; ++sectionIndex) {
      const int physicalButtonIndex = groupIndex * 2 + sectionIndex;
      const Rect section = sectionIndex == 0 ? Rect{group.x, group.y, seam, group.height}
                                             : Rect{group.x + seam, group.y, group.width - seam, group.height};
      const bool pressed = gpio.isPressed(static_cast<uint8_t>(physicalButtonIndex));
      anyPressed = anyPressed || pressed;

      // Visual-only pressed state: sample the hardware while composing the
      // activity frame without scheduling a second action or touching events.
      if (pressed) {
        fillSparseRoundedRect(renderer, Rect{section.x + 1, section.y + 1, section.width - 2, section.height - 2},
                              buttonHintCornerRadius - 1);
      }

      if (labels[physicalButtonIndex] && labels[physicalButtonIndex][0] != '\0') {
        const auto label =
            renderer.truncatedText(hintFontId, labels[physicalButtonIndex], section.width - 6, EpdFontFamily::REGULAR);
        const int textWidth = renderer.getTextWidth(hintFontId, label.c_str(), EpdFontFamily::REGULAR);
        const int textY = section.y + std::max(1, (section.height - renderer.getLineHeight(hintFontId)) / 2);
        renderer.drawText(hintFontId, section.x + (section.width - textWidth) / 2, textY, label.c_str(), true,
                          EpdFontFamily::REGULAR);
      }
    }

    // Draw the shared outline and seam last so both remain crisp when
    // either half is shown in its pressed state.
    renderer.drawRoundedRect(group.x, group.y, group.width, group.height, 1, buttonHintCornerRadius, true);
    const int dividerX = group.x + seam;
    renderer.drawLine(dividerX, group.y + 3, dividerX, group.y + group.height - 4, true);
  }

  UITheme::getInstance().markButtonHintsPressed(anyPressed);
  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (!SETTINGS.showButtonHints) return;

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  constexpr int topButtonY = 345;
  const char* labels[] = {topBtn, bottomBtn};
  const int x = screenWidth - buttonMargin - buttonWidth;

  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
    renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
    renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
  }

  if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
  }

  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
    renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                      topButtonY + 2 * buttonHeight - 1);
    renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
  }

  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topButtonY + i * buttonHeight;
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = x + (buttonWidth - textHeight) / 2;
      const int textY = y + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
    }
  }
}

int BaseTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  return contentHeight / rowHeight;
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<UIAccessory(int index)>& rowAccessory,
                         const std::function<bool(int index)>& rowSection) const {
  (void)rowAccessory;
  int rowHeight =
      (rowSubtitle != nullptr) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;

  int pageStartIndex = selectedIndex / pageItems * pageItems;
  if (rowSection && selectedIndex >= 0) {
    int sectionStart = selectedIndex;
    while (sectionStart > 0 && !rowSection(sectionStart)) --sectionStart;
    pageStartIndex = std::min(sectionStart, std::max(0, itemCount - pageItems));
  }

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  // Draw selection
  int contentWidth = rect.width - 5;
  if (selectedIndex >= 0) {
    renderer.fillRect(rect.x, rect.y + (selectedIndex - pageStartIndex) * rowHeight - 2, rect.width, rowHeight);
  }
  constexpr int minValueGap = 10;

  // Draw all items
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i - pageStartIndex) * rowHeight;

    if (rowSection && rowSection(i)) {
      const std::string sectionName = rowTitle(i);
      const auto section = renderer.truncatedText(SCRIPT_FONT_ID, sectionName.c_str(),
                                                  contentWidth - BaseMetrics::values.contentSidePadding * 2);
      const int sectionWidth = renderer.getTextWidth(SCRIPT_FONT_ID, section.c_str());
      const bool sectionRtl = BidiUtils::startsWithRtl(sectionName.c_str());
      const int sectionX = sectionRtl ? rect.x + contentWidth - BaseMetrics::values.contentSidePadding - sectionWidth
                                      : rect.x + BaseMetrics::values.contentSidePadding;
      renderer.drawText(SCRIPT_FONT_ID, sectionX, itemY + 4, section.c_str());
      continue;
    }

    int rowTextWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2;
    std::string valueText;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        int maxValW = std::max(0, rowTextWidth - 40 - minValueGap);
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxValW);
        int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + minValueGap;
        rowTextWidth -= valueWidth;
      }
    }

    auto itemName = rowTitle(i);
    auto font = UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), rowTextWidth);
    const bool rowRtl = BidiUtils::startsWithRtl(itemName.c_str());
    const int titleWidth = renderer.getTextWidth(font, item.c_str());
    const int titleX = rowRtl ? rect.x + contentWidth - BaseMetrics::values.contentSidePadding - titleWidth
                              : rect.x + BaseMetrics::values.contentSidePadding;
    renderer.drawText(font, titleX, itemY, item.c_str(), i != selectedIndex);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int lineH = renderer.getLineHeight(font);
      const int tx = titleX;
      for (int py = itemY; py < itemY + lineH; py++)
        for (int px = tx; px < tx + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      if (!subtitleText.empty()) {
        auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
        const int subtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, subtitle.c_str());
        const int subtitleX = BidiUtils::startsWithRtl(subtitleText.c_str())
                                  ? rect.x + contentWidth - BaseMetrics::values.contentSidePadding - subtitleWidth
                                  : rect.x + BaseMetrics::values.contentSidePadding;
        renderer.drawText(SMALL_FONT_ID, subtitleX, itemY + 22, subtitle.c_str(), i != selectedIndex);
      }
    }

    if (!valueText.empty()) {
      const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      int valueY = itemY;
      if (rowSubtitle != nullptr) {
        valueY = itemY + 10;
      }
      const int valueX = rowRtl ? rect.x + BaseMetrics::values.contentSidePadding
                                : rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth;
      renderer.drawText(UI_10_FONT_ID, valueX, valueY, valueText.c_str(), i != selectedIndex);
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  const bool primaryHeader = rect.y <= BaseMetrics::values.topPadding;
  const int batteryGroupWidth = primaryHeader ? UITheme::getInstance().getSystemBatteryOverlayWidth(renderer) : 0;

  if (title) {
    const int padding = batteryGroupWidth + BaseMetrics::values.contentSidePadding;
    auto truncatedTitle =
        renderer.truncatedText(HEADER_FONT_ID, title, std::max(0, rect.width - padding * 2), EpdFontFamily::REGULAR);
    renderer.drawCenteredText(HEADER_FONT_ID, rect.y + 5, truncatedTitle.c_str());
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(
        SMALL_FONT_ID, subtitle, rect.width - BaseMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - BaseMetrics::values.contentSidePadding - truncatedSubtitleWidth, subtitleY,
                      truncatedSubtitle.c_str(), true);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int maxListValueWidth = 200;

  const bool rtl = label && BidiUtils::startsWithRtl(label);
  int secondarySpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto secondary = renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    const int secondaryWidth = renderer.getTextWidth(SMALL_FONT_ID, secondary.c_str());
    const int secondaryX = rtl ? rect.x + BaseMetrics::values.contentSidePadding
                               : rect.x + rect.width - BaseMetrics::values.contentSidePadding - secondaryWidth;
    renderer.drawText(SMALL_FONT_ID, secondaryX, rect.y + 7, secondary.c_str());
    secondarySpace += secondaryWidth + 10;
  }

  auto heading =
      renderer.truncatedText(UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - secondarySpace,
                             EpdFontFamily::REGULAR);
  const int headingWidth = renderer.getTextWidth(UI_12_FONT_ID, heading.c_str());
  const int headingX = rtl ? rect.x + rect.width - BaseMetrics::values.contentSidePadding - headingWidth
                           : rect.x + BaseMetrics::values.contentSidePadding;
  renderer.drawText(UI_12_FONT_ID, headingX, rect.y, heading.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = BaseMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

void BaseTheme::drawSelection(const GfxRenderer& renderer, const Rect rect) const {
  fillSparseRoundedRect(renderer, rect, selectionCornerRadius);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, selectionCornerRadius, true);
}

void BaseTheme::drawPageDots(const GfxRenderer& renderer, const int selectedPage, const int pageCount) const {
  if (pageCount <= 1) return;
  constexpr int dotSize = 9;
  constexpr int dotGap = 11;
  const int totalWidth = pageCount * dotSize + (pageCount - 1) * dotGap;
  const int startX = (renderer.getScreenWidth() - totalWidth) / 2;
  const int y = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 42;
  for (int i = 0; i < pageCount; i++) {
    if (i == selectedPage) {
      renderer.fillRoundedRect(startX + i * (dotSize + dotGap), y, dotSize, dotSize, dotSize / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(startX + i * (dotSize + dotGap), y, dotSize, dotSize, 1, dotSize / 2, true, true, true,
                               true, true);
    }
  }
}

void BaseTheme::drawFooterCounter(GfxRenderer& renderer, const int selectedIndex, const int itemCount,
                                  const char* status) const {
  if (itemCount <= 0) return;
  char counter[32];
  snprintf(counter, sizeof(counter), "%d / %d", selectedIndex + 1, itemCount);
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Forced portrait, like the button hints directly below it: the counter
  // belongs to the physical bottom edge, not to the rotated content.
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - footerCounterTopOffset;
  const int contentLeft = metrics.contentSidePadding;
  const int contentRight = renderer.getScreenWidth() - metrics.contentSidePadding;
  const int counterWidth = renderer.getTextWidth(SMALL_FONT_ID, counter);

  if (status && status[0] != '\0') {
    constexpr int footerGap = 12;
    const int statusMaxWidth = std::max(0, contentRight - contentLeft - counterWidth - footerGap);
    const auto statusText = renderer.truncatedText(SMALL_FONT_ID, status, statusMaxWidth);
    const int statusWidth = renderer.getTextWidth(SMALL_FONT_ID, statusText.c_str());
    const int statusX = I18N.isRtl() ? contentRight - statusWidth : contentLeft;
    const int counterX = I18N.isRtl() ? contentLeft : contentRight - counterWidth;
    renderer.drawText(SMALL_FONT_ID, statusX, y, statusText.c_str());
    renderer.drawText(SMALL_FONT_ID, counterX, y, counter);
    renderer.setOrientation(origOrientation);
    return;
  }

  // Follows the interface language, not the counter's own digits, which are
  // direction-neutral and would always have resolved to left.
  const int x = I18N.isRtl() ? contentRight - counterWidth : contentLeft;
  renderer.drawText(SMALL_FONT_ID, x, y, counter);
  renderer.setOrientation(origOrientation);
}

void BaseTheme::drawDivider(const GfxRenderer& renderer, const int x1, const int x2, const int y) const {
  renderer.drawLine(x1, y, x2, y, 1, true);
}

void BaseTheme::drawEmptyState(const GfxRenderer& renderer, const Rect content, const char* message, const char* detail,
                               const bool script) const {
  if (!message || message[0] == '\0' || content.height <= 0) return;
  const int messageFontId = script ? SCRIPT_FONT_ID : UI_10_FONT_ID;
  const int maxWidth = std::max(0, content.width - BaseMetrics::values.contentSidePadding * 2);
  auto messageLines = renderer.wrappedText(messageFontId, message, maxWidth, 3);
  if (messageLines.empty()) return;

  const int messageLineHeight = renderer.getLineHeight(messageFontId);
  const int detailLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  std::vector<std::string> detailLines;
  if (detail && detail[0] != '\0') {
    detailLines = renderer.wrappedText(UI_10_FONT_ID, detail, maxWidth, 2);
  }
  const int blockHeight = static_cast<int>(messageLines.size()) * messageLineHeight +
                          static_cast<int>(detailLines.size()) * detailLineHeight;
  // Sit slightly above the middle: a block centred exactly reads as low on a tall
  // portrait panel with a header above it.
  int lineY = content.y + std::max(0, (content.height - blockHeight) * 2 / 5);
  for (const auto& line : messageLines) {
    const int lineWidth = renderer.getTextWidth(messageFontId, line.c_str());
    renderer.drawText(messageFontId, content.x + (content.width - lineWidth) / 2, lineY, line.c_str());
    lineY += messageLineHeight;
  }
  for (const auto& line : detailLines) {
    const int lineWidth = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
    renderer.drawText(UI_10_FONT_ID, content.x + (content.width - lineWidth) / 2, lineY, line.c_str());
    lineY += detailLineHeight;
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int maxPopupWidth = std::max(120, renderer.getScreenWidth() - metrics.contentSidePadding * 4);
  const int maxTextWidth = std::max(40, maxPopupWidth - marginX * 2);
  auto lines = renderer.wrappedText(UI_12_FONT_ID, message, maxTextWidth, 4, popupFontFamily);
  if (lines.empty()) lines.emplace_back("");

  const int textWidth = std::accumulate(
      lines.cbegin(), lines.cend(), 0, [&renderer, popupFontFamily](const int widest, const std::string& line) {
        return std::max(widest, renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), popupFontFamily));
      });
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int textHeight = lineHeight * static_cast<int>(lines.size());
  const int w = std::min(maxPopupWidth, std::max(180, textWidth + marginX * 2));
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  const int preferredY = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int y = std::clamp(preferredY, metrics.topPadding + metrics.headerHeight,
                           renderer.getScreenHeight() - h - metrics.buttonHintsHeight - metrics.verticalSpacing);

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius,
                             metrics.popupTextInverted ? Color::Black : Color::White);
    renderer.drawRoundedRect(x, y, w, h, metrics.popupFrameThickness, metrics.popupCornerRadius, true);
  } else {
    renderer.fillRect(x, y, w, h, metrics.popupTextInverted);
    renderer.drawRect(x, y, w, h, metrics.popupFrameThickness, true);
  }

  for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
    const auto& line = lines[lineIndex];
    const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), popupFontFamily);
    const int textX = x + (w - lineWidth) / 2;
    const int textY = y + marginY + metrics.popupTextBaselineOffsetY + static_cast<int>(lineIndex) * lineHeight;
    renderer.drawText(UI_12_FONT_ID, textX, textY, line.c_str(), !metrics.popupTextInverted, popupFontFamily);
  }
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, std::string title, const int paddingBottom, const int textYOffset,
                              const bool fillMargin, const bool isPageBookmarked) const {
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const bool showStatusBarTextLane = statusBarTextLaneVisible();

  // Draw Progress Text
  const auto screenHeight = renderer.getScreenHeight();
  auto textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom - 4;

  const int leftClusterX = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  const int rightClusterX = renderer.getScreenWidth() - metrics.statusBarHorizontalMargin - orientedMarginRight;
  int leftClusterWidth = 0;
  int rightClusterWidth = 0;

  if (SETTINGS.statusBarBookProgressPercentage || SETTINGS.statusBarChapterPageCount) {
    // Right aligned text for progress counter
    char progressStr[32];

    if (SETTINGS.statusBarBookProgressPercentage && SETTINGS.statusBarChapterPageCount) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %.0f%%", currentPage, pageCount, bookProgress);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%.0f%%", bookProgress);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", currentPage, pageCount);
    }

    int progressTextWidth = renderer.getTextWidth(MICRO_FONT_ID, progressStr);
    renderer.drawText(MICRO_FONT_ID, rightClusterX - progressTextWidth, textY, progressStr);

    rightClusterWidth += progressTextWidth;
  }

  // Draw Progress Bar
  if (SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    const int barMarginLeft = fillMargin ? 0 : orientedMarginLeft;
    const int barMarginRight = fillMargin ? 0 : orientedMarginRight;
    const int progressBarMaxWidth = renderer.getScreenWidth() - barMarginLeft - barMarginRight;
    const int progressBarY = renderer.getScreenHeight() - orientedMarginBottom -
                             ((SETTINGS.statusBarProgressBarThickness + 1) * 2) - paddingBottom + (fillMargin ? 1 : 0);
    size_t progress;
    if (SETTINGS.statusBarProgressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      // Chapter progress
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    const int barHeight =
        ((SETTINGS.statusBarProgressBarThickness + 1) * 2) + (fillMargin ? orientedMarginBottom - 1 : 0);
    renderer.fillRect(barMarginLeft, progressBarY, barWidth, barHeight, true);
  }

  // Draw Battery
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;

  if (SETTINGS.showBatteryIndicator && SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer,
                        Rect{leftClusterX + leftClusterWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage, MICRO_FONT_ID);
    int batteryWidth = metrics.batteryWidth;

    if (showBatteryPercentage) {
      const uint16_t percentage = powerManager.getBatteryPercentage();
      // width of icon + spacing + text for layout purposes
      batteryWidth +=
          batteryPercentSpacing + renderer.getTextWidth(MICRO_FONT_ID, (std::to_string(percentage) + "%").c_str());
    }

    leftClusterWidth += batteryWidth;
  }

  // Draw hardware RTC time on X3 or the current software time on X4.
  if (SETTINGS.statusBarClock && halClock.hasValidTime()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      int clockTextWidth = renderer.getTextWidth(MICRO_FONT_ID, timeBuf);
      int clockX = 0;
      // Position to the left or right of the progress text (with a small gap)
      if (SETTINGS.statusBarClock == CrossPointSettings::STATUS_BAR_CLOCK_LEFT) {
        clockX = leftClusterX + leftClusterWidth + (leftClusterWidth > 0 ? 10 : 0);
        leftClusterWidth += clockTextWidth + 10;
      } else if (SETTINGS.statusBarClock == CrossPointSettings::STATUS_BAR_CLOCK_RIGHT) {
        clockX = rightClusterX - rightClusterWidth - (rightClusterWidth > 0 ? 10 : 0) - clockTextWidth;
        rightClusterWidth += clockTextWidth + 10;
      }
      renderer.drawText(MICRO_FONT_ID, clockX, textY, timeBuf);
    }
  }

  // Draw Bookmark
  if (showStatusBarTextLane && isPageBookmarked) {
    const int bookmarkGap = leftClusterWidth > 0 ? bookmarkStatusIconGap : 0;
    const int bookmarkX = leftClusterX + leftClusterWidth + bookmarkGap;
    const int bookmarkY = textY + 3;
    drawBookmarkStatusIcon(renderer, bookmarkX, bookmarkY);
    leftClusterWidth += bookmarkStatusIconWidth + bookmarkGap;
  }

  // Draw Title
  if (!title.empty()) {
    textY -= textYOffset;
    // Centered chapter title text
    // Page width minus existing content with 30px padding on each side
    const int rendererableScreenWidth =
        renderer.getScreenWidth() - (metrics.statusBarHorizontalMargin * 2) - orientedMarginLeft - orientedMarginRight;

    const int titleMarginLeft = leftClusterWidth + 30;
    const int titleMarginRight = rightClusterWidth + 30;

    // Attempt to center title on the screen, but if title is too wide then later we will center it within the
    // available space.
    int titleMarginLeftAdjusted = std::max(titleMarginLeft, titleMarginRight);
    int availableTitleSpace = rendererableScreenWidth - 2 * titleMarginLeftAdjusted;

    int titleWidth;
    titleWidth = renderer.getTextWidth(MICRO_FONT_ID, title.c_str());
    if (titleWidth > availableTitleSpace) {
      // Not enough space to center on the screen, center it within the remaining space instead
      availableTitleSpace = rendererableScreenWidth - titleMarginLeft - titleMarginRight;
      titleMarginLeftAdjusted = titleMarginLeft;
    }
    if (titleWidth > availableTitleSpace) {
      title = renderer.truncatedText(MICRO_FONT_ID, title.c_str(), availableTitleSpace);
      titleWidth = renderer.getTextWidth(MICRO_FONT_ID, title.c_str());
    }

    renderer.drawText(MICRO_FONT_ID,
                      titleMarginLeftAdjusted + metrics.statusBarHorizontalMargin + orientedMarginLeft +
                          (availableTitleSpace - titleWidth) / 2,
                      textY, title.c_str());
  }
}

void BaseTheme::drawReaderMessage(const GfxRenderer& renderer, const char* message, const bool script) const {
  if (!message || message[0] == '\0') return;
  // Script styles quiet reader moments; error messages stay structural.
  const int fontId = script ? SCRIPT_FONT_ID : UI_12_FONT_ID;
  const auto style = script ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
  const int maxWidth = std::max(0, renderer.getScreenWidth() - BaseMetrics::values.contentSidePadding * 2);
  const auto lines = renderer.wrappedText(fontId, message, maxWidth, 3, style);
  if (lines.empty()) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  // Measured against the live viewport, so this stays centred in every orientation
  // and shifts with the reader's status bar instead of ignoring it.
  const int available = renderer.getScreenHeight() - UITheme::getStatusBarHeight();
  int y = std::max(0, (available - static_cast<int>(lines.size()) * lineHeight) / 2);
  for (const auto& line : lines) {
    renderer.drawCenteredText(fontId, y, line.c_str(), true, style);
    y += lineHeight;
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                                const char* secondaryLabel, const KeyboardKeyType keyType,
                                const bool inactiveSelection) const {
  (void)inactiveSelection;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int cr = metrics.keyboardKeyCornerRadius;
  const bool isSpecialKey = keyType == KeyboardKeyType::Shift || keyType == KeyboardKeyType::Mode ||
                            keyType == KeyboardKeyType::Del || keyType == KeyboardKeyType::Space ||
                            keyType == KeyboardKeyType::Ok || keyType == KeyboardKeyType::Disabled;

  if (isSelected) {
    if (inactiveSelection) {
      // Cursor editing owns the input focus, but the keyboard position must
      // remain easy to find when focus returns to the grid.
      if (cr > 0) {
        renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 3, cr, true);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 3);
      }
    } else if (cr > 0) {
      renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::Black);
    } else {
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
    }
  } else {
    if (metrics.keyboardFillUnselected) {
      if (keyType == KeyboardKeyType::Disabled) {
        if (cr > 0) {
          renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
        } else {
          renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
        }
      } else {
        if (cr > 0) {
          renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::White);
        } else {
          renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
        }
      }
    }

    const bool shouldDrawOutline =
        (metrics.keyboardDrawSpecialOutlineWhenUnselected && isSpecialKey) || metrics.keyboardOutlineAllUnselected;
    if (shouldDrawOutline) {
      if (cr > 0) {
        renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, cr, true);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
      }
    }
  }

  const bool drawBlackContent = !isSelected || inactiveSelection;

  if (keyType == KeyboardKeyType::Space) {
    const int lineHalfWidth = rect.width * 3 / 10;
    const int centerX = rect.x + rect.width / 2;
    const int lineY = rect.y + rect.height / 2 + 3;
    renderer.drawLine(centerX - lineHalfWidth, lineY, centerX + lineHalfWidth, lineY, 3, drawBlackContent);
    return;
  }

  if (keyType == KeyboardKeyType::Del) {
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;
    const int arrowLen = rect.width / 4;
    const int arrowHead = std::max(metrics.keyboardMinArrowHeadSize, arrowLen / 2);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX + arrowLen / 2, centerY, 3, drawBlackContent);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY - arrowHead, 3,
                      drawBlackContent);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY + arrowHead, 3,
                      drawBlackContent);
    return;
  }

  if (label == nullptr || label[0] == '\0') {
    return;
  }

  const bool hasSecondary = secondaryLabel != nullptr && secondaryLabel[0] != '\0';
  // Pinned one slot below the shifted scale, and the secondary label to MICRO: a
  // 10-column grid of single characters gains nothing from larger type and has no
  // room for it.
  // A word key can be wider than its cap ("SHIFT", Turkish "Tamam"): drop just
  // that key's label to MICRO before letting it spill over the outline.
  int keyFontId = UI_10_FONT_ID;
  int itemWidth = renderer.getTextWidth(keyFontId, label);
  if (itemWidth > rect.width - 4) {
    keyFontId = MICRO_FONT_ID;
    itemWidth = renderer.getTextWidth(keyFontId, label);
  }
  const auto fitted = renderer.truncatedText(keyFontId, label, rect.width - 4);
  if (fitted.size() != strlen(label)) {
    itemWidth = renderer.getTextWidth(keyFontId, fitted.c_str());
  }
  const int textX = rect.x + std::max(2, (rect.width - itemWidth) / 2);
  const int textY = rect.y + (rect.height - renderer.getLineHeight(keyFontId)) / 2;

  renderer.drawText(keyFontId, textX, textY, fitted.c_str(), drawBlackContent);

  if (hasSecondary) {
    const int secWidth = renderer.getTextWidth(MICRO_FONT_ID, secondaryLabel);
    renderer.drawText(MICRO_FONT_ID, rect.x + rect.width - secWidth - metrics.keyboardSecondaryLabelRightPadding,
                      rect.y + metrics.keyboardSecondaryLabelTopPadding, secondaryLabel, drawBlackContent);
  }
}
