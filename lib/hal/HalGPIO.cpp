#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <esp_sleep.h>

HalGPIO gpio;

void HalGPIO::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  if (BoardConfig::ACTIVE.usbDetect >= 0) {
    pinMode(BoardConfig::ACTIVE.usbDetect, INPUT);
    lastUsbConnected.store(digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH, std::memory_order_relaxed);
  }
  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();

  uint8_t pressed = 0;
  uint8_t released = 0;
  for (uint8_t button = BTN_BACK; button <= BTN_POWER; ++button) {
    const uint8_t mask = static_cast<uint8_t>(1U << button);
    if (inputMgr.wasPressed(button)) pressed |= mask;
    if (inputMgr.wasReleased(button)) released |= mask;
  }

  // X4 Pro touch fallback for InkPointX's button-oriented activities. The
  // native GT911 events are converted into the same queued logical navigation
  // primitives, so every existing screen remains reachable while dedicated
  // coordinate-aware touch screens can still read the raw methods below.
  uint8_t touchButton = UINT8_MAX;
  if (inputMgr.wasHomeKeyTapped()) {
    touchButton = BTN_BACK;
  } else if (inputMgr.wasHomeKeyLongPressed()) {
    touchButton = BTN_CONFIRM;
  } else {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    if (inputMgr.wasSwipe(x0, y0, x1, y1)) {
      const float dx = x1 - x0;
      const float dy = y1 - y0;
      touchButton = (abs(dx) >= abs(dy)) ? (dx < 0.0f ? BTN_DOWN : BTN_UP) : (dy < 0.0f ? BTN_DOWN : BTN_UP);
    } else if (inputMgr.wasTouchTap(x0, y0)) {
      touchButton = x0 < 0.33f ? BTN_UP : (x0 > 0.67f ? BTN_DOWN : BTN_CONFIRM);
    }
  }
  enqueueInputEdges(pressed, released);

  if (touchButton <= BTN_POWER) {
    const uint8_t mask = static_cast<uint8_t>(1U << touchButton);
    // Queue the press first so enqueueInputEdges can fold the matching release
    // into this exact click even when an older click for the same button is
    // still waiting behind an e-ink refresh.
    enqueueInputEdges(mask, 0);
    enqueueInputEdges(0, mask);
  }
  updatePowerState();
}

HalGPIO::InputEvent* HalGPIO::newestPendingPress(const uint8_t buttonMask) {
  // A release normally arrives while its corresponding press is still waiting
  // behind an e-ink refresh. Fold the pair into one logical click so draining a
  // backlog does not spend a second Activity tick on a no-op release frame.
  for (uint8_t offset = 0; offset < inputEventCount; ++offset) {
    const uint8_t reverse = static_cast<uint8_t>(inputEventCount - 1U - offset);
    const uint8_t index = static_cast<uint8_t>((inputEventHead + reverse) % INPUT_EVENT_QUEUE_SIZE);
    InputEvent& event = inputEvents[index];
    if ((event.pressed & buttonMask) != 0 && (event.released & buttonMask) == 0) return &event;
  }
  return nullptr;
}

void HalGPIO::enqueueInputEdges(const uint8_t pressed, const uint8_t released) {
  if (pressed == 0 && released == 0) return;

  uint8_t unmatchedReleases = released;
  for (uint8_t button = BTN_BACK; button <= BTN_POWER; ++button) {
    const uint8_t mask = static_cast<uint8_t>(1U << button);
    if ((unmatchedReleases & mask) == 0) continue;
    if (InputEvent* event = newestPendingPress(mask)) {
      event->released |= mask;
      unmatchedReleases &= static_cast<uint8_t>(~mask);
    }
  }

  if (pressed == 0 && unmatchedReleases == 0) return;
  if (inputEventCount >= INPUT_EVENT_QUEUE_SIZE) {
    if (!inputQueueOverflowLogged) {
      inputQueueOverflowLogged = true;
      LOG_ERR("INPUT", "Input queue full; dropping edge pressed=0x%02x released=0x%02x", pressed, unmatchedReleases);
    }
    return;
  }

  const uint8_t tail = static_cast<uint8_t>((inputEventHead + inputEventCount) % INPUT_EVENT_QUEUE_SIZE);
  inputEvents[tail] = {pressed, unmatchedReleases};
  ++inputEventCount;
}

void HalGPIO::consumeInputEvent() {
  if (inputEventCount == 0) return;
  inputEvents[inputEventHead] = {};
  inputEventHead = static_cast<uint8_t>((inputEventHead + 1U) % INPUT_EVENT_QUEUE_SIZE);
  --inputEventCount;
  if (inputEventCount == 0) inputQueueOverflowLogged = false;
}

void HalGPIO::clearInputEvents() {
  while (inputEventCount != 0) consumeInputEvent();
  inputEventHead = 0;
  inputQueueOverflowLogged = false;
}

bool HalGPIO::pendingInputIsNavigationOnly() const {
  if (inputEventCount == 0) return false;
  constexpr uint8_t navigationMask = (1U << BTN_LEFT) | (1U << BTN_RIGHT) | (1U << BTN_UP) | (1U << BTN_DOWN);
  const InputEvent& event = inputEvents[inputEventHead];
  const uint8_t edges = event.pressed | event.released;
  return edges != 0 && (edges & static_cast<uint8_t>(~navigationMask)) == 0;
}

#if LOG_LEVEL >= 2
void HalGPIO::enqueueSyntheticClick(const uint8_t buttonIndex) {
  if (buttonIndex > BTN_POWER) return;
  const uint8_t mask = static_cast<uint8_t>(1U << buttonIndex);
  enqueueInputEdges(mask, 0);
  enqueueInputEdges(0, mask);
}
#endif

void HalGPIO::updatePowerState() {
  usbStateChanged.store(false, std::memory_order_relaxed);
  if (BoardConfig::ACTIVE.usbDetect < 0) return;
  const bool connected = digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  const bool previous = lastUsbConnected.exchange(connected, std::memory_order_relaxed);
  usbStateChanged.store(connected != previous, std::memory_order_relaxed);
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged.load(std::memory_order_relaxed); }
bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }
bool HalGPIO::wasPressed(const uint8_t buttonIndex) const {
  return inputEventCount != 0 && (inputEvents[inputEventHead].pressed & (1U << buttonIndex)) != 0;
}
bool HalGPIO::wasAnyPressed() const { return inputEventCount != 0 && inputEvents[inputEventHead].pressed != 0; }
bool HalGPIO::wasReleased(const uint8_t buttonIndex) const {
  return inputEventCount != 0 && (inputEvents[inputEventHead].released & (1U << buttonIndex)) != 0;
}
bool HalGPIO::wasAnyReleased() const { return inputEventCount != 0 && inputEvents[inputEventHead].released != 0; }
unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }
unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }
bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }
bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }
bool HalGPIO::wasHomeKeyPressed() const { return inputMgr.wasHomeKeyPressed(); }
bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }
bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }
bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }
bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }
bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }
bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}
bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }
unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }
bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}
bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

bool HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    return true;
  }

  const unsigned long calibration = millis();
  const unsigned long calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;
  const auto start = millis();
  inputMgr.update();
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (!inputMgr.isPressed(BTN_POWER)) {
    return false;
  }

  do {
    delay(10);
    inputMgr.update();
  } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
  return inputMgr.getPowerButtonHeldTime() >= calibratedDuration;
}

bool HalGPIO::isUsbConnected() const { return lastUsbConnected.load(std::memory_order_relaxed); }

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN) {
    return WakeupReason::AfterFlash;
  }
  // X4 Pro has no validated VBUS-detect pin. A cold boot may be a normal power
  // button start or the first reset after flashing; both must stay awake. Only
  // a real deep-sleep GPIO wake is subjected to the hold-duration check above.
  return WakeupReason::Other;
}
