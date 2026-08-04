#include "HalFrontlight.h"

#include <Logging.h>

namespace {
uint8_t clampPercent(const uint8_t value) { return value > 100 ? 100 : value; }
}  // namespace

HalFrontlight& HalFrontlight::getInstance() {
  static HalFrontlight instance;
  return instance;
}

void HalFrontlight::begin() {
  if (!manager.present()) return;

  manager.begin();
  manager.setColorTemperature(warm);
  manager.setBrightness(lit ? level : 0);
  LOG_INF("LIGHT", "Frontlight: %s, level=%u%%, warmth=%u%%", lit ? "on" : "off", level, warm);
}

// The X4 Pro test image must leave the OEM NVS partition byte-for-byte intact
// so stock app1 remains recoverable with all factory calibration. Frontlight
// state is therefore session-only in this dedicated branch.
void HalFrontlight::save() const {}

void HalFrontlight::setOn(const bool on, const bool persist) {
  if (!manager.present()) return;
  lit = on;
  manager.setBrightness(lit ? level : 0);
  if (persist) save();
}

void HalFrontlight::toggle() { setOn(!lit); }

void HalFrontlight::setBrightness(const uint8_t percent, const bool persist) {
  if (!manager.present()) return;
  level = clampPercent(percent);
  if (lit) manager.setBrightness(level);
  if (persist) save();
}

void HalFrontlight::setWarmth(const uint8_t percent, const bool persist) {
  if (!manager.present()) return;
  warm = clampPercent(percent);
  manager.setColorTemperature(warm);
  if (persist) save();
}

void HalFrontlight::prepareForSleep() {
  if (!manager.present()) return;
  // Preserve the user's persisted on/off preference while guaranteeing that
  // both PWM outputs are low throughout deep sleep.
  manager.setBrightness(0);
}
