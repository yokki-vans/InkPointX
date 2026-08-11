#include "LyraTheme.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/lucide_ui.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 10;
constexpr int cornerRadius = 14;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 32;
constexpr int newBadgeTextPadding = 3;
constexpr int newBadgeVerticalPadding = 1;
constexpr char newBadgeText[] = "NEW";
// Row accessories (chevron, check, star) scale with the type.
constexpr int accessoryIconSize = 24;
// Big enough to read at arm's length, and the two states differ by fill, not
// just knob side: ON is a solid track with an inverted knob, OFF an outline
// with a black knob.
constexpr int toggleWidth = 50;
constexpr int toggleHeight = 28;
// Space kept clear on the right of every list and menu for the scroll indicator,
// reserved unconditionally so a selection box does not shift when a list grows
// past one page.
int scrollGutterWidth(const ThemeMetrics& metrics) { return metrics.scrollBarWidth + metrics.scrollBarRightOffset; }
// Vertical breathing room between a row's bounds and its selection outline.
// Shared by lists and menu tiles so the selection is the same height on both.
constexpr int selectionVerticalInset = 4;

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == mainMenuIconSize) {
    switch (icon) {
      case UIIcon::Folder:
        return LucideFolder32;
      case UIIcon::Text:
      case UIIcon::File:
        return LucideFileText32;
      case UIIcon::Image:
        return LucideImage32;
      case UIIcon::Book:
      case UIIcon::BookNew:
        return LucideBookOpen32;
      case UIIcon::Recent:
      case UIIcon::Clock:
        return LucideClock32;
      case UIIcon::Settings:
        return LucideSettings32;
      case UIIcon::Transfer:
        return LucideSend32;
      case UIIcon::Library:
        return LucideLibrary32;
      case UIIcon::Wifi:
        return LucideWifi32;
      case UIIcon::Hotspot:
        return LucideHotspot32;
      case UIIcon::Bookmark:
        return LucideBookmark32;
      // A star, matching the marker drawn on a favourited row. A bookmark glyph
      // here meant one concept was shown with two different symbols: a bookmark
      // reads as "saved position", which is a different feature.
      case UIIcon::Favorite:
        return LucideStar32;
      case UIIcon::Interface:
        return LucideInterface32;
      case UIIcon::Power:
        return LucidePower32;
      case UIIcon::Reading:
        return LucideReading32;
      case UIIcon::Controls:
        return LucideControls32;
      case UIIcon::Files:
        return LucideFiles32;
      case UIIcon::NetworkSync:
        return LucideNetwork32;
      case UIIcon::System:
        return LucideSystem32;
      case UIIcon::ReaderPage:
        return LucidePage32;
      case UIIcon::ReaderChapters:
        return LucideChapters32;
      case UIIcon::ReaderDictionary:
        return LucideDictionary32;
      case UIIcon::ReaderFootnotes:
        return LucideFootnotes32;
      case UIIcon::ReaderStats:
        return LucideStats32;
      case UIIcon::ReaderRotate:
        return LucideRotate32;
      case UIIcon::ReaderAutoTurn:
        return LucideAutoTurn32;
      case UIIcon::ReaderQr:
        return LucideQr32;
      case UIIcon::ReaderHome:
        return LucideHome32;
      case UIIcon::ReaderTrash:
        return LucideTrash32;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

void drawHairline(const GfxRenderer& renderer, int x1, int x2, int y) {
  for (int x = x1; x <= x2; x += 2) renderer.drawPixel(x, y, true);
}

int centeredInkTextY(const GfxRenderer& renderer, const int fontId, const char* text, const int centerY,
                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  int inkTop = 0;
  int inkHeight = 0;
  if (renderer.getTextInkBounds(fontId, text, &inkTop, &inkHeight, style)) {
    return centerY - inkTop - inkHeight / 2;
  }
  return centerY - renderer.getLineHeight(fontId) / 2;
}

bool isBookIcon(const UIIcon icon) { return icon == UIIcon::Book || icon == UIIcon::BookNew; }

int newBadgeWidth(const GfxRenderer& renderer) {
  return renderer.getTextWidth(MICRO_FONT_ID, newBadgeText) + newBadgeTextPadding * 2;
}

void drawNewBadge(const GfxRenderer& renderer, const int iconX, const int iconY, const int iconSize) {
  const int textHeight = renderer.getLineHeight(MICRO_FONT_ID);
  const int width = newBadgeWidth(renderer);
  const int height = textHeight + newBadgeVerticalPadding * 2;
  const int x = iconX + (iconSize - width) / 2;
  const int y = iconY + iconSize - height;
  renderer.fillRoundedRect(x, y, width, height, height / 2, Color::Black);
  renderer.drawText(MICRO_FONT_ID, x + newBadgeTextPadding, y + newBadgeVerticalPadding, newBadgeText, false);
}

int accessoryWidth(const UIAccessory accessory) {
  return accessory == UIAccessory::ToggleOff || accessory == UIAccessory::ToggleOn ? toggleWidth : accessoryIconSize;
}

void drawAccessory(const GfxRenderer& renderer, const UIAccessory accessory, const int x, const int y, bool rtl) {
  switch (accessory) {
    case UIAccessory::Chevron:
      renderer.drawIcon(rtl ? LucideChevronLeft24 : LucideChevronRight24, x, y, accessoryIconSize, accessoryIconSize);
      break;
    case UIAccessory::Check:
      renderer.drawIcon(LucideCheck24, x, y, accessoryIconSize, accessoryIconSize);
      break;
    case UIAccessory::Favorite:
      renderer.drawIcon(LucideStar24, x, y, accessoryIconSize, accessoryIconSize);
      break;
    case UIAccessory::ToggleOff:
    case UIAccessory::ToggleOn: {
      const bool on = accessory == UIAccessory::ToggleOn;
      const int knobSize = toggleHeight - 8;
      if (on) {
        renderer.fillRoundedRect(x, y, toggleWidth, toggleHeight, toggleHeight / 2, Color::Black);
        renderer.fillRoundedRect(x + toggleWidth - knobSize - 4, y + 4, knobSize, knobSize, knobSize / 2, Color::White);
      } else {
        renderer.drawRoundedRect(x, y, toggleWidth, toggleHeight, 1, toggleHeight / 2, true);
        renderer.fillRoundedRect(x + 4, y + 4, knobSize, knobSize, knobSize / 2, Color::Black);
      }
      break;
    }
    case UIAccessory::None:
      break;
  }
}

}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();
  // rect is the body now, terminal excluded, so the inset is two per side.
  const int maxFill = rect.width - 4;
  const int fillWidth = charging ? maxFill : std::clamp(static_cast<int>(percentage) * maxFill / 100, 0, maxFill);
  if (fillWidth > 0) {
    renderer.fillRoundedRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4, 1, Color::Black);
  }
  if (charging) drawBatteryLightningBolt(renderer, rect.x + 3, rect.y + 2);
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  constexpr int titleTop = 11;
  const int contentLeft = rect.x + metrics.contentSidePadding;
  int contentRight = rect.x + rect.width - metrics.contentSidePadding;
  const bool primaryHeader = rect.y <= metrics.topPadding;
  if (primaryHeader && SETTINGS.showBatteryIndicator) {
    contentRight -= UITheme::getInstance().getSystemBatteryOverlayWidth(renderer) + hPaddingInSelection;
  }
  const bool rtl = title && BidiUtils::startsWithRtl(title);
  int secondaryWidth = 0;
  if (subtitle) {
    if (auto* cache = renderer.getFontCacheManager()) cache->warmGlyphCache(SMALL_FONT_ID, subtitle);
    // Give the value whatever the title does not need, rather than a fixed half.
    // A short title ("Books") next to a long value ("Sorting: Recently opened")
    // otherwise truncated the value to nothing but its own label, which is the one
    // part the user already knows.
    const int available = std::max(0, contentRight - contentLeft);
    const int titleNeeds = title ? renderer.getTextWidth(SCRIPT_FONT_ID, title) : 0;
    const int secondaryBudget = std::max(available / 2, available - titleNeeds - hPaddingInSelection);
    const auto secondary = renderer.truncatedText(SMALL_FONT_ID, subtitle, std::min(available, secondaryBudget));
    secondaryWidth = renderer.getTextWidth(SMALL_FONT_ID, secondary.c_str());
    const int secondaryX = rtl ? contentLeft : contentRight - secondaryWidth;
    renderer.drawText(SMALL_FONT_ID, secondaryX, rect.y + titleTop + 5, secondary.c_str());
  }
  if (title) {
    if (auto* cache = renderer.getFontCacheManager()) cache->warmGlyphCache(SCRIPT_FONT_ID, title);
    const int maxWidth = std::max(0, contentRight - contentLeft - secondaryWidth - hPaddingInSelection);
    const auto heading = renderer.truncatedText(SCRIPT_FONT_ID, title, maxWidth);
    const int headingWidth = renderer.getTextWidth(SCRIPT_FONT_ID, heading.c_str());
    const int headingX = rtl ? contentRight - headingWidth : contentLeft;
    const int headingY = centeredInkTextY(renderer, SCRIPT_FONT_ID, heading.c_str(), rect.y + (rect.height - 2) / 2);
    renderer.drawText(SCRIPT_FONT_ID, headingX, headingY, heading.c_str());
  }
  drawHairline(renderer, contentLeft, rect.x + rect.width - metrics.contentSidePadding, rect.y + rect.height - 2);
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool rtl = label && BidiUtils::startsWithRtl(label);
  int secondarySpace = metrics.contentSidePadding;
  if (rightLabel) {
    auto secondary = renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    const int secondaryWidth = renderer.getTextWidth(SMALL_FONT_ID, secondary.c_str());
    const int secondaryX =
        rtl ? rect.x + metrics.contentSidePadding : rect.x + rect.width - metrics.contentSidePadding - secondaryWidth;
    renderer.drawText(SMALL_FONT_ID, secondaryX, rect.y + 9, secondary.c_str());
    secondarySpace += secondaryWidth + hPaddingInSelection;
  }

  auto heading = renderer.truncatedText(UI_10_FONT_ID, label, rect.width - metrics.contentSidePadding - secondarySpace,
                                        EpdFontFamily::BOLD);
  const int headingWidth = renderer.getTextWidth(UI_10_FONT_ID, heading.c_str());
  const int headingX =
      rtl ? rect.x + rect.width - metrics.contentSidePadding - headingWidth : rect.x + metrics.contentSidePadding;
  renderer.drawText(UI_10_FONT_ID, headingX, rect.y + 8, heading.c_str(), true, EpdFontFamily::BOLD);

  drawHairline(renderer, rect.x + metrics.contentSidePadding, rect.x + rect.width - metrics.contentSidePadding,
               rect.y + rect.height - 1);
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return contentHeight / rowHeight;
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<UIAccessory(int index)>& rowAccessory,
                         const std::function<bool(int index)>& rowSection) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = rowSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  const int titleFontId = rowSubtitle ? UI_12_FONT_ID : UI_10_FONT_ID;
  const int subtitleFontId = UI_10_FONT_ID;
  const int pageItems = std::max(1, rect.height / rowHeight);
  int pageStartIndex = selectedIndex >= 0 ? selectedIndex / pageItems * pageItems : 0;
  if (rowSection && selectedIndex >= 0) {
    int sectionStart = selectedIndex;
    while (sectionStart > 0 && !rowSection(sectionStart)) --sectionStart;
    pageStartIndex = std::min(sectionStart, std::max(0, itemCount - pageItems));
  }
  const int pageEndIndex = std::min(itemCount, pageStartIndex + pageItems);

  // Batch the visible labels before drawing. Warm both styles for the whole
  // page, not only the currently selected row: moving the selection changes
  // which title is bold, and SD-card fonts otherwise replace their bold mini
  // cache on every key press.
  std::string regularText;
  std::string boldText;
  std::string subtitleGlyphs;
  std::string compactSubtitleGlyphs;
  std::string sectionGlyphs;
  bool hasNewBadge = false;
  for (int i = pageStartIndex; i < pageEndIndex; ++i) {
    const std::string title = rowTitle(i);
    if (rowSection && rowSection(i)) {
      sectionGlyphs.append(title).push_back('\n');
      continue;
    }
    const UIIcon icon = rowIcon ? rowIcon(i) : UIIcon::None;
    regularText.append(title).push_back('\n');
    boldText.append(title).push_back('\n');
    hasNewBadge = hasNewBadge || icon == UIIcon::BookNew;
    if (rowSubtitle) {
      std::string& glyphs = isBookIcon(icon) ? compactSubtitleGlyphs : subtitleGlyphs;
      glyphs.append(rowSubtitle(i)).push_back('\n');
    }
    if (rowValue) {
      const std::string value = rowValue(i);
      regularText.append(value).push_back('\n');
      if (highlightValue) boldText.append(value).push_back('\n');
    }
  }
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->warmGlyphCache(titleFontId, regularText.c_str(), 1U << EpdFontFamily::REGULAR);
    cache->warmGlyphCache(titleFontId, boldText.c_str(), 1U << EpdFontFamily::BOLD);
    cache->warmGlyphCache(subtitleFontId, subtitleGlyphs.c_str(), 1U << EpdFontFamily::REGULAR);
    cache->warmGlyphCache(SMALL_FONT_ID, compactSubtitleGlyphs.c_str(), 1U << EpdFontFamily::REGULAR);
    cache->warmGlyphCache(SCRIPT_FONT_ID, sectionGlyphs.c_str(), 1U << EpdFontFamily::REGULAR);
    if (hasNewBadge) cache->warmGlyphCache(MICRO_FONT_ID, newBadgeText, 1U << EpdFontFamily::REGULAR);
  }

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = std::max(28, (scrollAreaHeight * pageItems) / itemCount);
    const int scrollBarY =
        rowSection
            ? rect.y + ((scrollAreaHeight - scrollBarHeight) * pageStartIndex) / std::max(1, itemCount - pageItems)
            : rect.y + ((scrollAreaHeight - scrollBarHeight) * (selectedIndex / pageItems)) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - metrics.scrollBarRightOffset;
    renderer.fillRoundedRect(scrollBarX - metrics.scrollBarWidth, scrollBarY, metrics.scrollBarWidth, scrollBarHeight,
                             metrics.scrollBarWidth / 2, Color::Black);
  }

  // Reserve the scroll gutter whether or not a scrollbar is showing. Sizing it
  // conditionally moved every selection box 10 px sideways the moment a library
  // grew past one page, and the old `: 1` fallback also left list selections one
  // pixel narrower than the menu tiles they sit next to on Home.
  const int contentWidth = rect.width - scrollGutterWidth(metrics);
  if (selectedIndex >= 0) {
    const int selectedY = rect.y + (selectedIndex - pageStartIndex) * rowHeight;
    drawSelection(renderer,
                  Rect{rect.x + metrics.contentSidePadding, selectedY + selectionVerticalInset,
                       contentWidth - metrics.contentSidePadding * 2, rowHeight - selectionVerticalInset * 2});
  }

  const int rowLeft = rect.x + metrics.contentSidePadding + hPaddingInSelection;
  const int rowRight = rect.x + contentWidth - metrics.contentSidePadding - hPaddingInSelection;
  const int iconSize = rowIcon ? listIconSize : 0;
  const int titleLineHeight = renderer.getLineHeight(titleFontId);

  for (int i = pageStartIndex; i < pageEndIndex; ++i) {
    const int itemY = rect.y + (i - pageStartIndex) * rowHeight;
    const std::string itemName = rowTitle(i);

    if (rowSection && rowSection(i)) {
      const bool sectionRtl = BidiUtils::startsWithRtl(itemName.c_str());
      const int maxSectionWidth = std::max(0, rowRight - rowLeft);
      const auto section = renderer.truncatedText(SCRIPT_FONT_ID, itemName.c_str(), maxSectionWidth);
      const int sectionWidth = renderer.getTextWidth(SCRIPT_FONT_ID, section.c_str());
      const int sectionX = sectionRtl ? rowRight - sectionWidth : rowLeft;
      const int sectionY = centeredInkTextY(renderer, SCRIPT_FONT_ID, section.c_str(), itemY + rowHeight / 2);
      renderer.drawText(SCRIPT_FONT_ID, sectionX, sectionY, section.c_str());

      // A short labelled rule makes the heading read as a section boundary,
      // while keeping the list visually lighter than another selectable tile.
      constexpr int labelRuleGap = 12;
      const int ruleY = itemY + rowHeight / 2;
      if (sectionRtl) {
        drawHairline(renderer, rowLeft, sectionX - labelRuleGap, ruleY);
      } else {
        drawHairline(renderer, sectionX + sectionWidth + labelRuleGap, rowRight, ruleY);
      }
      continue;
    }

    const bool rowRtl = BidiUtils::startsWithRtl(itemName.c_str());
    const UIIcon icon = rowIcon ? rowIcon(i) : UIIcon::None;
    const bool showNewBadge = icon == UIIcon::BookNew;
    const UIAccessory accessory = rowAccessory ? rowAccessory(i) : UIAccessory::None;
    const int accessoryW = accessory == UIAccessory::None ? 0 : accessoryWidth(accessory);
    const int accessoryH =
        accessory == UIAccessory::None
            ? 0
            : ((accessory == UIAccessory::ToggleOff || accessory == UIAccessory::ToggleOn) ? toggleHeight
                                                                                           : accessoryIconSize);
    const int accessorySpace = accessory == UIAccessory::None ? 0 : accessoryW + hPaddingInSelection;

    const int iconX = rowRtl ? rowRight - iconSize : rowLeft;
    const int accessoryX = rowRtl ? rowLeft : rowRight - accessoryW;
    const int leadingSpace = iconSize > 0 ? iconSize + hPaddingInSelection : 0;
    const int textLeft = rowLeft + (rowRtl ? accessorySpace : leadingSpace);
    const int textRight = rowRight - (rowRtl ? leadingSpace : accessorySpace);

    std::string valueText;
    int valueWidth = 0;
    if (rowValue) {
      valueText = rowValue(i);
      const int valueMaxWidth = std::min(maxListValueWidth, std::max(0, (textRight - textLeft) / 2));
      const auto valueStyle = highlightValue && i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), valueMaxWidth, valueStyle);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str(), valueStyle);
    }

    const int valueGap = valueWidth > 0 ? hPaddingInSelection : 0;
    const int rowTextWidth = std::max(0, textRight - textLeft - valueWidth - valueGap);
    const int textLaneLeft = rowRtl ? textLeft + valueWidth + valueGap : textLeft;
    const int textLaneRight = rowRtl ? textRight : textRight - valueWidth - valueGap;
    const auto titleStyle = i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const auto item = renderer.truncatedText(titleFontId, itemName.c_str(), rowTextWidth, titleStyle);
    const int itemWidth = renderer.getTextWidth(titleFontId, item.c_str(), titleStyle);
    const int titleX = rowRtl ? textLaneRight - itemWidth : textLaneLeft;
    // Anchor the title to the top only when this row actually has a subtitle under
    // it. Keying off the callback instead meant a book with no author kept the
    // top-anchored title and an empty band below it, which read as a gap in the
    // rhythm rather than as a row with less to say.
    const bool rowHasSubtitle = rowSubtitle && !rowSubtitle(i).empty();
    const int titleY = rowHasSubtitle
                           ? itemY + 8
                           : centeredInkTextY(renderer, titleFontId, item.c_str(), itemY + rowHeight / 2, titleStyle);
    renderer.drawText(titleFontId, titleX, titleY, item.c_str(), true,
                      i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      for (int py = titleY; py < titleY + titleLineHeight; ++py)
        for (int px = titleX; px < titleX + itemWidth; ++px)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon) {
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      const int iconY = itemY + (rowHeight - iconSize) / 2;
      if (iconBitmap) renderer.drawIcon(iconBitmap, iconX, iconY, iconSize, iconSize);
      if (showNewBadge) {
        drawNewBadge(renderer, iconX, iconY, iconSize);
      }
    }

    if (rowSubtitle) {
      const std::string subtitleText = rowSubtitle(i);
      const int rowSubtitleFontId = isBookIcon(icon) ? SMALL_FONT_ID : subtitleFontId;
      const int subtitleLineHeight = renderer.getLineHeight(rowSubtitleFontId);
      const auto subtitle = renderer.truncatedText(rowSubtitleFontId, subtitleText.c_str(), rowTextWidth);
      const int subtitleWidth = renderer.getTextWidth(rowSubtitleFontId, subtitle.c_str());
      const int subtitleX =
          BidiUtils::startsWithRtl(subtitleText.c_str()) ? textLaneRight - subtitleWidth : textLaneLeft;
      renderer.drawText(rowSubtitleFontId, subtitleX, itemY + rowHeight - subtitleLineHeight - 7, subtitle.c_str(),
                        true);
    }

    if (!valueText.empty()) {
      const int valueX = rowRtl ? textLeft : textRight - valueWidth;
      const auto valueStyle = highlightValue && i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int valueY =
          centeredInkTextY(renderer, UI_10_FONT_ID, valueText.c_str(), itemY + rowHeight / 2, valueStyle);
      renderer.drawText(UI_10_FONT_ID, valueX, valueY, valueText.c_str(), true, valueStyle);
    }

    if (accessory != UIAccessory::None) {
      drawAccessory(renderer, accessory, accessoryX, itemY + (rowHeight - accessoryH) / 2, rowRtl);
    }

    const bool rowIsSelected = i == selectedIndex;
    const bool nextRowIsSelected = i + 1 == selectedIndex;
    if (!rowIsSelected && !nextRowIsSelected && i + 1 < pageEndIndex) {
      drawHairline(renderer, textLeft, textRight, itemY + rowHeight - 1);
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  BaseTheme::drawButtonHints(renderer, btn1, btn2, btn3, btn4);
}

void LyraTheme::drawDivider(const GfxRenderer& renderer, const int x1, const int x2, const int y) const {
  drawHairline(renderer, x1, x2, y);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (!SETTINGS.showButtonHints) return;

  const int screenWidth = renderer.getScreenWidth();
  const int buttonWidth = UITheme::getInstance().getMetrics().sideButtonHintsWidth;
  constexpr int buttonHeight = 78;  // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    // Own the strip. The legend is chrome drawn on top of whatever the screen
    // already painted, and a header hairline ends 8 px inside it — so without
    // clearing first, that rule runs straight through the label.
    const int stripTop = topHintButtonY;
    const int stripHeight = buttonHeight * 2 + 5;
    renderer.fillRect(x, stripTop, buttonWidth, stripHeight, false);

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  std::string labels;
  for (int i = 0; i < buttonCount; ++i) {
    const std::string label = buttonLabel(i);
    labels.append(label).push_back('\n');
  }
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->warmGlyphCache(UI_12_FONT_ID, labels.c_str(), 1U << EpdFontFamily::REGULAR);
    cache->warmGlyphCache(UI_12_FONT_ID, labels.c_str(), 1U << EpdFontFamily::BOLD);
  }

  for (int i = 0; i < buttonCount; ++i) {
    // Same right edge and same vertical inset as drawList, so a selection moving
    // between menu tiles and list rows on one screen does not visibly jog.
    const int tileWidth = rect.width - metrics.contentSidePadding * 2 - scrollGutterWidth(metrics);
    const Rect tileRect =
        Rect{rect.x + metrics.contentSidePadding, rect.y + i * (metrics.menuRowHeight + metrics.menuSpacing), tileWidth,
             metrics.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      drawSelection(renderer, Rect{tileRect.x, tileRect.y + selectionVerticalInset, tileRect.width,
                                   tileRect.height - selectionVerticalInset * 2});
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const bool rtl = BidiUtils::startsWithRtl(label);
    const int iconX = rtl ? tileRect.x + tileRect.width - 16 - mainMenuIconSize : tileRect.x + 16;
    const int accessoryX = rtl ? tileRect.x + 12 : tileRect.x + tileRect.width - 28;
    const int textLeft = tileRect.x + 16 + (rtl ? 22 : mainMenuIconSize + hPaddingInSelection);
    const int textRight = tileRect.x + tileRect.width - 16 - (rtl ? mainMenuIconSize + hPaddingInSelection : 22);
    const auto labelStyle = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    if (rowIcon) {
      const UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap)
        renderer.drawIcon(iconBitmap, iconX, tileRect.y + (tileRect.height - mainMenuIconSize) / 2, mainMenuIconSize,
                          mainMenuIconSize);
    }

    const auto truncated = renderer.truncatedText(UI_12_FONT_ID, label, std::max(0, textRight - textLeft), labelStyle);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, truncated.c_str(), labelStyle);
    const int textX = rtl ? textRight - labelWidth : textLeft;
    const int textY =
        centeredInkTextY(renderer, UI_12_FONT_ID, truncated.c_str(), tileRect.y + tileRect.height / 2, labelStyle);
    renderer.drawText(UI_12_FONT_ID, textX, textY, truncated.c_str(), true, labelStyle);
    drawAccessory(renderer, UIAccessory::Chevron, accessoryX, tileRect.y + (tileRect.height - accessoryIconSize) / 2,
                  rtl);

    const bool nextSelected = i + 1 == selectedIndex;
    if (!selected && !nextSelected && i + 1 < buttonCount) {
      drawHairline(renderer, textLeft, textRight, tileRect.y + tileRect.height + metrics.menuSpacing / 2);
    }
  }
}
