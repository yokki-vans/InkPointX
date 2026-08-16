#pragma once

#include <string>

#include "achievements/AchievementModel.h"
#include "activities/Activity.h"

class AchievementUnlockActivity final : public Activity {
  std::string message;
  AchievementId achievementId = AchievementId::FirstPage;
  uint16_t unlockCount = 0;
  bool achievementPopup = false;

 public:
  AchievementUnlockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string message)
      : Activity("AchievementUnlock", renderer, mappedInput), message(std::move(message)) {}
  AchievementUnlockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, AchievementId achievementId,
                            uint16_t unlockCount)
      : Activity("AchievementUnlock", renderer, mappedInput),
        achievementId(achievementId),
        unlockCount(unlockCount),
        achievementPopup(true) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
