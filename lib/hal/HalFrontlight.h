#pragma once

#include <FrontlightManager.h>

#include <cstdint>

// X4 Pro frontlight facade. It is capability-gated by FreeInk and therefore a
// harmless no-op on X3/X4 builds. User state lives in NVS because settings.json
// is on the removable card and the light must be available before any UI opens.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance();

  void begin();
  bool present() const { return manager.present(); }
  bool isOn() const { return lit; }
  uint8_t brightness() const { return level; }
  uint8_t warmth() const { return warm; }

  void setOn(bool on, bool persist = true);
  void toggle();
  void setBrightness(uint8_t percent, bool persist = true);
  void setWarmth(uint8_t percent, bool persist = true);
  void prepareForSleep();

 private:
  HalFrontlight() = default;
  void save() const;

  FrontlightManager manager;
  uint8_t level = 55;
  uint8_t warm = 50;
  bool lit = false;
};

#define Frontlight HalFrontlight::getInstance()
