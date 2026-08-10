#pragma once
#include <I18n.h>

#include <string>

#include "activities/Activity.h"
#include "util/LaunchInputGuard.h"

class QrDisplayActivity final : public Activity {
 public:
  explicit QrDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& textPayload)
      : Activity("QrDisplay", renderer, mappedInput), textPayload(textPayload) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string textPayload;
  LaunchInputGuard inputGuard_;
};
