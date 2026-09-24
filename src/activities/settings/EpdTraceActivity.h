#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

#if defined(INKPOINTX_DEVICE_QA)

// Field viewer for the per-frame e-ink decision trace (EpdTrace).
// Reads /.crosspoint/epd.log straight off the SD card (the card never leaves
// the device) and shows it on screen so a session can be reported by photo.
// Falls back to the in-RAM ring when the SD read comes up empty.
class EpdTraceActivity final : public Activity {
  std::vector<std::string> displayLines;  // wrapped, ready to draw
  int scrollOffset = 0;
  int pageSize = 1;

 public:
  explicit EpdTraceActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("EpdTrace", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // INKPOINTX_DEVICE_QA
