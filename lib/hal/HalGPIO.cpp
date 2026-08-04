#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Preferences.h>
#include <SPI.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

HalGPIO gpio;

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
// Only a positively fingerprinted X3 is persisted. An all-zero I2C probe is
// consistent with X4, but also with a temporarily unpowered/stuck X3 bus, so it
// is never safe to turn that absence into a permanent X4 decision. Bump the key
// to discard X4 assumptions written by older firmware.
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det3";  // 0=unknown, 2=x3

// Charging state is only presentation data on X3; polling it more frequently
// needlessly keeps the gauge and I2C peripheral active while reading.
constexpr unsigned long X3_POWER_POLL_MS = 5000;
constexpr uint8_t X3_POWER_STABLE_SAMPLES = 2;

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3) {
    LOG_INF("HW", "Using positively fingerprinted cached device type: X3");
    return HalGPIO::DeviceType::X3;
  }

  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }
  // X4 is the geometry-safe fallback for a dual-device image, but absence of
  // X3 I2C responses is not positive proof of X4. Never cache this decision.
  LOG_INF("HW", "No positive X3 fingerprint; using X4 fallback for this boot only");
  return HalGPIO::DeviceType::X4;
}

}  // namespace
#endif

void HalGPIO::begin() {
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // X3 GPIO13 powers the SD rail. Release a hold left by deep sleep before
  // probing the display because the card shares SCLK/MOSI with the panel.
  if (deviceIsX3()) {
    BoardConfig::releaseSdRail();
  }

  // Production batches use either the original controller (UC8253/SSD1677)
  // or its UltraChip sibling (UC8279/UC8179). Resolve it before SPI/display init.
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
    BoardConfig::releaseSdRail();
  }

  SPI.begin(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.display.mosi,
            BoardConfig::ACTIVE.display.cs);
#else
  _deviceType = BoardConfig::isX4Pro() ? DeviceType::X4Pro : DeviceType::X4;
#endif

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

  if (!deviceIsX3()) {
    if (BoardConfig::ACTIVE.usbDetect < 0) return;
    const bool connected = digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
    const bool previous = lastUsbConnected.exchange(connected, std::memory_order_relaxed);
    usbStateChanged.store(connected != previous, std::memory_order_relaxed);
    return;
  }

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  const unsigned long now = millis();
  if (x3PowerLastPollMs != 0 && (now - x3PowerLastPollMs) < X3_POWER_POLL_MS) return;
  x3PowerLastPollMs = now;

  // The X3 exposes no dedicated charger/VBUS-present pin. Current flowing into
  // the cell is useful for the icon only; it is explicitly not a cable-presence
  // signal (a full battery can report zero while still plugged in).
  static const BatteryMonitor battery;
  const BatteryMonitor::Status status = battery.readStatus();
  if (!status.chargingKnown) {
    x3PowerCandidateSamples = 0;
    return;  // transient I2C failure: retain the last known indication
  }
  const bool charging = status.charging;

  if (!x3PowerSampleInitialized || charging != x3PowerCandidate) {
    x3PowerSampleInitialized = true;
    x3PowerCandidate = charging;
    x3PowerCandidateSamples = 1;
    return;
  }

  if (x3PowerCandidateSamples < X3_POWER_STABLE_SAMPLES) ++x3PowerCandidateSamples;
  if (x3PowerCandidateSamples < X3_POWER_STABLE_SAMPLES) return;

  const bool previous = lastUsbConnected.exchange(x3PowerCandidate, std::memory_order_relaxed);
  usbStateChanged.store(previous != x3PowerCandidate, std::memory_order_relaxed);
#endif
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

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro;
}

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
  // Only X4 has a hardware USB-present signal. On X3 the cached value denotes
  // active cell charging and must never decide whether a cold boot or flash was
  // caused by USB power.
  const bool usbConnected = deviceIsX4() && isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
