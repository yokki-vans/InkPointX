#include "HalFrontlight.h"

#include <Logging.h>
#include <Preferences.h>

namespace {
constexpr char NVS_NAMESPACE[] = "ipxlight";
constexpr char NVS_LEVEL[] = "level";
constexpr char NVS_WARMTH[] = "warmth";
constexpr char NVS_ON[] = "on";

uint8_t clampPercent(const uint8_t value) { return value > 100 ? 100 : value; }
}  // namespace

HalFrontlight& HalFrontlight::getInstance() {
  static HalFrontlight instance;
  return instance;
}

void HalFrontlight::begin() {
  if (!manager.present()) return;

  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    level = clampPercent(prefs.getUChar(NVS_LEVEL, level));
    warm = clampPercent(prefs.getUChar(NVS_WARMTH, warm));
    lit = prefs.getBool(NVS_ON, false);
    prefs.end();
  }

  manager.begin();
  manager.setColorTemperature(warm);
  manager.setBrightness(lit ? level : 0);
  LOG_INF("LIGHT", "Frontlight: %s, level=%u%%, warmth=%u%%", lit ? "on" : "off", level, warm);
}

void HalFrontlight::save() const {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putUChar(NVS_LEVEL, level);
  prefs.putUChar(NVS_WARMTH, warm);
  prefs.putBool(NVS_ON, lit);
  prefs.end();
}

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
