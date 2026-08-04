#pragma once

#include <Arduino.h>
#include <InputManager.h>

#include <atomic>

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  // Physical e-ink refreshes take roughly half a second. The Arduino loop
  // keeps sampling during that time, but the active Activity is deliberately
  // locked against the render task. InputManager edges only live for one
  // sample, so without this queue every click made mid-refresh disappears.
  struct InputEvent {
    uint8_t pressed = 0;
    uint8_t released = 0;
  };
  static constexpr uint8_t INPUT_EVENT_QUEUE_SIZE = 32;
  InputEvent inputEvents[INPUT_EVENT_QUEUE_SIZE]{};
  uint8_t inputEventHead = 0;
  uint8_t inputEventCount = 0;
  bool inputQueueOverflowLogged = false;

  void enqueueInputEdges(uint8_t pressed, uint8_t released);
  InputEvent* newestPendingPress(uint8_t buttonMask);

  // The X4 Pro VBUS-detect GPIO is not confirmed. Keep this generic cache for
  // a future validated profile, but never infer cable presence from the gauge.
  std::atomic<bool> lastUsbConnected{false};
  std::atomic<bool> usbStateChanged{false};

  void updatePowerState();

 public:
  HalGPIO() = default;

  // Start the X4 Pro digital buttons and GT911 input backend.
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  // Removes the event currently exposed by wasPressed()/wasReleased(). Called
  // only after the active Activity has actually had a chance to consume it.
  void consumeInputEvent();
  void clearInputEvents();
  bool hasPendingInputEvent() const { return inputEventCount != 0; }
  // Safe to coalesce within one UI frame: directional buttons only. Back,
  // Confirm and Power remain one-per-main-loop because they can change screens
  // or trigger system actions.
  bool pendingInputIsNavigationOnly() const;
#if LOG_LEVEL >= 2
  void enqueueSyntheticClick(uint8_t buttonIndex);
#endif
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool hasTouch() const;
  bool hasHomeKey() const;
  bool wasHomeKeyPressed() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool isTouchHeldAt(float& nx, float& ny) const;
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;

  // Verify power button was held long enough after wakeup.
  // Returns false when the device should immediately return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check a future validated VBUS pin. The current X4 Pro profile has none and
  // therefore always returns false.
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
