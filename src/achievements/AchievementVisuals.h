#pragma once

#include <cstdint>

#include "AchievementModel.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;

UIIcon achievementIcon(AchievementId id);
const uint8_t* achievementIconBitmap(AchievementId id);
void drawAchievementMedallion(const GfxRenderer& renderer, AchievementId id, int x, int y, int size, bool unlocked);
