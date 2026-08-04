#pragma once

#include <Arduino.h>
#include <InputManager.h>

#include <atomic>

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  // Physical refreshes take roughly half a second on X3/X4. The Arduino loop
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

  // X4 has a real USB-detect GPIO. X3 does not: its best available signal is
  // the fuel-gauge current sign, which means "actively charging", not "VBUS is
  // present". Keep that slow I2C-derived indication cached here so rendering a
  // battery icon can never perform an I2C transaction and the main loop does
  // not hammer the gauge at 20-100 Hz.
  std::atomic<bool> lastUsbConnected{false};
  std::atomic<bool> usbStateChanged{false};
  bool x3PowerSampleInitialized = false;
  bool x3PowerCandidate = false;
  uint8_t x3PowerCandidateSamples = 0;
  unsigned long x3PowerLastPollMs = 0;

 public:
  enum class DeviceType : uint8_t { X4, X3, X4Pro };

 private:
#if FREEINK_DEVICE_X4PRO
  DeviceType _deviceType = DeviceType::X4Pro;
#else
  DeviceType _deviceType = DeviceType::X4;
#endif

  void updatePowerState();

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  inline bool deviceIsX4Pro() const { return _deviceType == DeviceType::X4Pro; }
  bool isXteinkDevice() const;

  // Start button GPIO and setup SPI for screen and SD card
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

  // Check if external power is observed. On X4 this is the hardware USB-detect
  // pin. On X3 there is no reliable VBUS signal, so this returns the cached,
  // debounced "actively charging" indication. It must not be used to classify
  // an X3 boot source.
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
