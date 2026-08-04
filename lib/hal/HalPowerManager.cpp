#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0 || modeMutex == nullptr) {
    return;  // invalid state
  }

  // Query WiFi before taking the local mutex; the WiFi stack has its own locks
  // and must never be called while holding ours.
  const bool wifiActive = WiFi.getMode() != WIFI_MODE_NULL;

  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const bool shouldUseLowPower = enabled && !wifiActive && normalSpeedLockCount == 0;
  if (shouldUseLowPower && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = true;

  } else if (!shouldUseLowPower && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      xSemaphoreGive(modeMutex);
      return;
    }
    isLowPower = false;
  }
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Shut down the X4 Pro touch/SD/frontlight rails after display.deepSleep().
  freeink::PowerManager::powerDownRailsForSleep();
  freeink::PowerManager::deepSleepUntilPowerButton();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  if (powerManager.modeMutex == nullptr) {
    LOG_ERR("PWR", "Lock requested before power manager initialization");
    return;
  }
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.normalSpeedLockCount == UINT16_MAX) {
    LOG_ERR("PWR", "Normal-speed lock count overflow");
    xSemaphoreGive(powerManager.modeMutex);
    return;
  }
  ++powerManager.normalSpeedLockCount;
  valid = true;
  xSemaphoreGive(powerManager.modeMutex);

  // Immediately restore normal CPU frequency if currently in low-power mode.
  // setPowerSaving() observes the count under the same mutex.
  powerManager.setPowerSaving(false);
}

HalPowerManager::Lock::~Lock() {
  if (!valid || powerManager.modeMutex == nullptr) return;
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (powerManager.normalSpeedLockCount == 0) {
    LOG_ERR("PWR", "Unbalanced normal-speed lock release");
  } else {
    --powerManager.normalSpeedLockCount;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
