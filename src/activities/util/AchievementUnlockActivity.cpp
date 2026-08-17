#include "AchievementUnlockActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "achievements/AchievementSystem.h"
#include "achievements/AchievementVisuals.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
void drawPopupScrim(const GfxRenderer& renderer) {
  for (int y = 0; y < renderer.getScreenHeight(); y += 2) {
    const int offset = ((y / 2) & 1) * 2;
    for (int x = offset; x < renderer.getScreenWidth(); x += 4) renderer.drawPixel(x, y, true);
  }
}

int drawCenteredLines(const GfxRenderer& renderer, const int font, const char* text, const int x, const int y,
                      const int width, const int maxLines, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const auto lines = renderer.wrappedText(font, text, width, maxLines, style);
  int lineY = y;
  const int lineHeight = renderer.getLineHeight(font);
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(font, line.c_str(), style);
    renderer.drawText(font, x + std::max(0, (width - lineWidth) / 2), lineY, line.c_str(), true, style);
    lineY += lineHeight;
  }
  return lineY;
}

void drawAchievementPopup(const GfxRenderer& renderer, const AchievementId id, const uint16_t count) {
  const int side = std::min({352, renderer.getScreenWidth() - 64, renderer.getScreenHeight() - 112});
  const int x = (renderer.getScreenWidth() - side) / 2;
  const int y = (renderer.getScreenHeight() - side) / 2;
  constexpr int radius = 22;

  drawPopupScrim(renderer);
  renderer.fillRoundedRect(x, y, side, side, radius, Color::White);
  renderer.drawRoundedRect(x, y, side, side, 2, radius, true);

  const char* eyebrow = tr(STR_ACHIEVEMENT_UNLOCKED);
  const int eyebrowFont =
      renderer.getTextWidth(UI_10_FONT_ID, eyebrow, EpdFontFamily::BOLD) <= side - 72 ? UI_10_FONT_ID : SMALL_FONT_ID;
  const std::string visibleEyebrow = renderer.truncatedText(eyebrowFont, eyebrow, side - 48, EpdFontFamily::BOLD);
  const int eyebrowWidth = renderer.getTextWidth(eyebrowFont, visibleEyebrow.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(eyebrowFont, x + (side - eyebrowWidth) / 2, y + 18, visibleEyebrow.c_str(), true,
                    EpdFontFamily::BOLD);
  const int headerDividerY = y + 56;
  renderer.drawLine(x + 22, headerDividerY, x + side - 23, headerDividerY, true);

  constexpr int medallionSize = 76;
  const int medallionX = x + (side - medallionSize) / 2;
  const int medallionY = y + 72;
  drawAchievementMedallion(renderer, id, medallionX, medallionY, medallionSize, true);

  if (count > 1) {
    char more[20];
    snprintf(more, sizeof(more), "+%u", static_cast<unsigned>(count - 1));
    const int pillW = renderer.getTextWidth(SMALL_FONT_ID, more, EpdFontFamily::BOLD) + 20;
    const int pillH = 24;
    const int pillX = medallionX + medallionSize - pillW / 2;
    const int pillY = medallionY - 4;
    renderer.fillRoundedRect(pillX, pillY, pillW, pillH, pillH / 2, Color::White);
    renderer.drawRoundedRect(pillX, pillY, pillW, pillH, 1, pillH / 2, true);
    renderer.drawText(
        SMALL_FONT_ID,
        pillX + std::max(0, (pillW - renderer.getTextWidth(SMALL_FONT_ID, more, EpdFontFamily::BOLD)) / 2), pillY + 3,
        more, true, EpdFontFamily::BOLD);
  }

  const int textX = x + 28;
  const int textW = side - 56;
  const std::string name = ACHIEVEMENTS.name(id);
  const std::string description = ACHIEVEMENTS.description(id);
  const auto nameLines = renderer.wrappedText(UI_12_FONT_ID, name.c_str(), textW, 2, EpdFontFamily::BOLD);
  const auto descriptionLines = renderer.wrappedText(UI_10_FONT_ID, description.c_str(), textW, 2);
  const int nameHeight = static_cast<int>(nameLines.size()) * renderer.getLineHeight(UI_12_FONT_ID);
  const int descriptionHeight = static_cast<int>(descriptionLines.size()) * renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int textGap = 7;
  const int textBlockHeight = nameHeight + textGap + descriptionHeight;
  const int textAreaTop = y + 164;
  const int footerY = y + side - 50;
  const int textAreaBottom = footerY - 16;
  int textY = textAreaTop + std::max(0, (textAreaBottom - textAreaTop - textBlockHeight) / 2);
  textY =
      drawCenteredLines(renderer, UI_12_FONT_ID, name.c_str(), textX, textY, textW, 2, EpdFontFamily::BOLD) + textGap;
  drawCenteredLines(renderer, UI_10_FONT_ID, description.c_str(), textX, textY, textW, 2);

  renderer.drawLine(x + 22, footerY, x + side - 23, footerY, true);
  const char* close = tr(STR_CLOSE);
  renderer.drawText(SMALL_FONT_ID, x + std::max(0, (side - renderer.getTextWidth(SMALL_FONT_ID, close)) / 2),
                    footerY + 14, close);
}
}  // namespace

void AchievementUnlockActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void AchievementUnlockActivity::loop() {
  if (mappedInput.hasPendingInputEvent()) finish();
}

void AchievementUnlockActivity::render(RenderLock&&) {
  // Deliberately keep the framebuffer from the activity underneath. The
  // shared popup supplies its own scrim and square card, so unlocking an
  // achievement feels like a transient notification instead of navigation.
  if (achievementPopup) {
    drawAchievementPopup(renderer, achievementId, unlockCount);
  } else {
    GUI.drawPopup(renderer, message.c_str());
  }
  renderer.displayBuffer();
}
