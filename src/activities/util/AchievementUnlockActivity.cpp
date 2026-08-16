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
    renderer.drawText(font, x + (width - lineWidth) / 2, lineY, line.c_str(), true, style);
    lineY += lineHeight;
  }
  return lineY;
}

void drawAchievementPopup(const GfxRenderer& renderer, const AchievementId id, const uint16_t count) {
  const int side = std::min({360, renderer.getScreenWidth() - 52, renderer.getScreenHeight() - 100});
  const int x = (renderer.getScreenWidth() - side) / 2;
  const int y = (renderer.getScreenHeight() - side) / 2;
  constexpr int radius = 24;

  drawPopupScrim(renderer);
  renderer.fillRoundedRect(x + 6, y + 7, side, side, radius, Color::LightGray);
  renderer.fillRoundedRect(x, y, side, side, radius, Color::White);
  renderer.drawRoundedRect(x, y, side, side, 1, radius, true);

  const char* eyebrow = tr(STR_ACHIEVEMENT_UNLOCKED);
  const int eyebrowWidth = renderer.getTextWidth(UI_10_FONT_ID, eyebrow, EpdFontFamily::BOLD);
  const int ruleY = y + 35;
  const int ruleGap = 12;
  const int ruleLeft = x + 24;
  const int ruleRight = x + side - 25;
  renderer.drawLine(ruleLeft, ruleY, std::max(ruleLeft, x + (side - eyebrowWidth) / 2 - ruleGap), ruleY, true);
  renderer.drawLine(std::min(ruleRight, x + (side + eyebrowWidth) / 2 + ruleGap), ruleY, ruleRight, ruleY, true);
  renderer.drawText(UI_10_FONT_ID, x + (side - eyebrowWidth) / 2, y + 20, eyebrow, true, EpdFontFamily::BOLD);

  constexpr int medallionSize = 82;
  drawAchievementMedallion(renderer, id, x + (side - medallionSize) / 2, y + 57, medallionSize, true);

  const int textX = x + 28;
  const int textW = side - 56;
  int textY = y + 158;
  const std::string name = ACHIEVEMENTS.name(id);
  textY = drawCenteredLines(renderer, UI_12_FONT_ID, name.c_str(), textX, textY, textW, 2, EpdFontFamily::BOLD) + 6;
  const std::string description = ACHIEVEMENTS.description(id);
  textY = drawCenteredLines(renderer, UI_10_FONT_ID, description.c_str(), textX, textY, textW, 2);

  if (count > 1) {
    char more[20];
    snprintf(more, sizeof(more), "+%u", static_cast<unsigned>(count - 1));
    const int pillW = renderer.getTextWidth(UI_10_FONT_ID, more, EpdFontFamily::BOLD) + 28;
    const int pillH = 30;
    const int pillX = x + (side - pillW) / 2;
    const int pillY = std::min(y + side - 79, textY + 8);
    renderer.drawRoundedRect(pillX, pillY, pillW, pillH, 1, pillH / 2, true);
    renderer.drawText(UI_10_FONT_ID,
                      pillX + (pillW - renderer.getTextWidth(UI_10_FONT_ID, more, EpdFontFamily::BOLD)) / 2, pillY + 4,
                      more, true, EpdFontFamily::BOLD);
  }

  const int footerY = y + side - 47;
  renderer.drawLine(x + 24, footerY, x + side - 25, footerY, true);
  const char* close = tr(STR_CLOSE);
  renderer.drawText(SMALL_FONT_ID, x + (side - renderer.getTextWidth(SMALL_FONT_ID, close)) / 2, footerY + 13, close);
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
