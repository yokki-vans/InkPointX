#include "AchievementUnlockActivity.h"

#include "components/UITheme.h"

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
  GUI.drawPopup(renderer, message.c_str());
  renderer.displayBuffer();
}
