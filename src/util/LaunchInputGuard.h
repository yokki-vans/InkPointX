#pragma once

#include <initializer_list>

#include "MappedInputManager.h"

// Quarantines the release edge that opened a child activity. A genuinely new
// press that starts after the child is installed is still accepted, including
// HALs that report its press and release in one input cycle.
class LaunchInputGuard final {
 public:
  using Button = MappedInputManager::Button;

  void reset() { armed_ = false; }

  bool allowsInput(const MappedInputManager& input, const std::initializer_list<Button> buttons) {
    if (armed_) return true;

    bool newPress = false;
    bool held = false;
    for (const Button button : buttons) {
      newPress = newPress || input.wasPressed(button);
      held = held || input.isPressed(button);
    }

    if (newPress) {
      armed_ = true;
      return true;
    }

    // Arm once the launch button is neutral, but discard this cycle so its
    // release-only edge cannot act on the newly opened activity.
    if (!held) armed_ = true;
    return false;
  }

 private:
  bool armed_ = false;
};
