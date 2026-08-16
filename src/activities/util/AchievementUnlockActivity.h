#pragma once

#include <string>

#include "activities/Activity.h"

class AchievementUnlockActivity final : public Activity {
  std::string message;

 public:
  AchievementUnlockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string message)
      : Activity("AchievementUnlock", renderer, mappedInput), message(std::move(message)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
