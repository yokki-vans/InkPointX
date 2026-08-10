#pragma once

#include "activities/Activity.h"
#include "util/LaunchInputGuard.h"

class DeviceInfoActivity final : public Activity {
 public:
  explicit DeviceInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DeviceInfo", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  LaunchInputGuard inputGuard_;
};
