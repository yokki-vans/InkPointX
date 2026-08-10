#include "DeviceInfoActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "components/UITheme.h"

void DeviceInfoActivity::onEnter() {
  Activity::onEnter();
  inputGuard_.reset();
  requestUpdate();
}

void DeviceInfoActivity::loop() {
  if (!inputGuard_.allowsInput(mappedInput,
                               {MappedInputManager::Button::Back, MappedInputManager::Button::Confirm})) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void DeviceInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_DEVICE_INFO));

  char battery[24];
  snprintf(battery, sizeof(battery), "%u%%", static_cast<unsigned>(powerManager.getBatteryPercentage()));
  const std::array<const char*, 5> labels = {tr(STR_DEVICE_MODEL), tr(STR_FIRMWARE_VERSION), tr(STR_DISPLAY),
                                             tr(STR_PROCESSOR), tr(STR_BATTERY)};
  const std::array<std::string, 5> values = {gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4", CROSSPOINT_VERSION,
                                             gpio.deviceIsX3() ? "792 \xc3\x97 528 e-ink" : "800 \xc3\x97 480 e-ink",
                                             "ESP32-C3", battery};

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, top, width, std::max(0, UITheme::getListContentBottom(renderer, false) - top)},
      static_cast<int>(labels.size()), -1, [&labels](const int index) { return std::string(labels[index]); }, nullptr,
      nullptr, [&values](const int index) { return values[index]; }, false);

  // Nothing here is selectable — "Select" promised a detail view that does
  // not exist. Confirm stays a hidden alias of Back.
  const auto buttonLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, buttonLabels.btn1, buttonLabels.btn2, buttonLabels.btn3, buttonLabels.btn4);
  renderer.displayBuffer();
}
