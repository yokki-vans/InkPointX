#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/LaunchInputGuard.h"

/**
 * Reference sheet for the reader's button gestures.
 *
 * The reading page deliberately shows no button legend, which keeps it clean but
 * leaves every reader gesture without an on-screen affordance — and the firmware
 * has no help screen anywhere else either. The page turn and the menu are found by
 * experiment; the holds are not. This screen is the one place that states them.
 *
 * Every label is an existing translated string, and the gesture column is built
 * from the user's own button mapping plus ASCII digits, so this screen needs no
 * new translations and stays correct after a remap.
 */
class ReaderGesturesActivity final : public Activity {
 public:
  explicit ReaderGesturesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderGestures", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator{};
  int selectorIndex = 0;
  LaunchInputGuard inputGuard_;
};
