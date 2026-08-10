#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  (void)type;
  LOG_DBG("UI", "Using InkPoint X theme");
  // The only production theme is stateless. Keep it in static storage instead
  // of allocating it during global initialization, where -fno-exceptions would
  // turn an unlikely OOM into an unreportable abort before setup().
  static LyraTheme lyraTheme;
  currentTheme = &lyraTheme;
  currentMetrics = LyraMetrics::values;
  if (SETTINGS.uiDensity == CrossPointSettings::UI_COMPACT) {
    currentMetrics.headerHeight = 60;
    currentMetrics.verticalSpacing = 6;
    currentMetrics.contentSidePadding = 16;
    currentMetrics.listRowHeight = 54;
    currentMetrics.listWithSubtitleRowHeight = 76;
    currentMetrics.menuRowHeight = 58;
    currentMetrics.subHeaderHeight = 44;
  }
  if (!SETTINGS.showButtonHints) {
    currentMetrics.buttonHintsHeight = 0;
    currentMetrics.sideButtonHintsWidth = 0;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasSubHeader,
                                     bool hasButtonHints, bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasSubHeader) {
    reservedHeight += metrics.subHeaderHeight;
  }
  if (hasButtonHints && SETTINGS.showButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints && SETTINGS.showButtonHints) {
        safeArea.height -= currentMetrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints && SETTINGS.showButtonHints) {
        safeArea.x += currentMetrics.buttonHintsHeight;
        safeArea.width -= currentMetrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints && SETTINGS.showButtonHints) {
        safeArea.y += currentMetrics.buttonHintsHeight;
        safeArea.height -= currentMetrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints && SETTINGS.showButtonHints) {
        safeArea.width -= currentMetrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

int UITheme::getListContentBottom(const GfxRenderer& renderer, const bool hasFooterCounter) {
  const ThemeMetrics& metrics = getInstance().getMetrics();
  int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (hasFooterCounter) {
    bottom -= BaseTheme::footerCounterTopOffset;
  }
  return bottom;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (!filename.empty() && filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
      FsHelpers::hasFb2Extension(filename) || FsHelpers::hasPdfExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
      FsHelpers::hasPngExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar =
      SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
      SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
      (SETTINGS.showBatteryIndicator && SETTINGS.statusBarBattery) ||
      SETTINGS.statusBarClock != CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getSystemBatteryOverlayWidth(const GfxRenderer& renderer) const {
  if (!SETTINGS.showBatteryIndicator) return 0;

  int width = currentMetrics.batteryWidth;
  if (SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS) {
    width += BaseTheme::batteryPercentSpacing + renderer.getTextWidth(BaseTheme::batteryPercentFontId, "100%");
  }
  return width;
}

void UITheme::drawSystemBatteryOverlay(const GfxRenderer& renderer) const {
  if (!SETTINGS.showBatteryIndicator) return;

  const bool showPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int iconX = renderer.getScreenWidth() - currentMetrics.contentSidePadding - currentMetrics.batteryWidth;
  const int textY = currentMetrics.topPadding + 14;
  clearSystemBatteryOverlay(renderer);
  currentTheme->drawBatteryRight(
      renderer, Rect{iconX, textY, currentMetrics.batteryWidth, currentMetrics.batteryHeight}, showPercentage);
}

void UITheme::clearSystemBatteryOverlay(const GfxRenderer& renderer) const {
  const int iconX = renderer.getScreenWidth() - currentMetrics.contentSidePadding - currentMetrics.batteryWidth;
  const int textY = currentMetrics.topPadding + 14;
  const int groupWidth = currentMetrics.batteryWidth + BaseTheme::batteryPercentSpacing +
                         renderer.getTextWidth(BaseTheme::batteryPercentFontId, "100%");
  const int clearLeft = iconX - (groupWidth - currentMetrics.batteryWidth) - 3;
  const int clearHeight =
      std::max(renderer.getLineHeight(BaseTheme::batteryPercentFontId), currentMetrics.batteryHeight + 6) + 6;

  // Clear the complete group so quick-resume sleep cannot retain the indicator.
  renderer.fillRect(clearLeft, textY - 3, groupWidth + 6, clearHeight, false);
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}
