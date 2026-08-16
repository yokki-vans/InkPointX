#include "FileInfoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
// Raw byte counts run to nine digits for a large PDF, which then got truncated.
// Report the same unit the rest of the firmware uses instead.
std::string formatFileSize(const size_t bytes) {
  char buffer[32];
  if (bytes >= 1024u * 1024u) {
    snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024u) {
    snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buffer, sizeof(buffer), "%u %s", static_cast<unsigned>(bytes), tr(STR_BYTES));
  }
  return buffer;
}
}  // namespace

void FileInfoActivity::onEnter() {
  Activity::onEnter();
  inputGuard_.reset();
  requestUpdate();
}

void FileInfoActivity::loop() {
  if (!inputGuard_.allowsInput(mappedInput, {MappedInputManager::Button::Back, MappedInputManager::Button::Confirm})) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void FileInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const std::array<std::string, 3> labels = {tr(STR_FILENAME), tr(STR_ITEM_TYPE), tr(STR_FILE_SIZE)};
  const size_t slash = path.find_last_of('/');
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::array<std::string, 3> values = {
      name,
      directory ? std::string(tr(STR_FOLDER)) : std::string(tr(STR_FILE)),
      directory ? std::string("-") : formatFileSize(size),
  };

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PROPERTIES));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(0, UITheme::getListContentBottom(renderer, false) - contentTop);
  // Two-line rows: label above, value below. Passing the value through drawList's
  // value lane clamped it to ~200 px, which truncated the very filename this
  // screen exists to show even though the row had 440 px available.
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(labels.size()), -1,
      [&labels](int index) { return labels[index]; }, [&values](int index) { return values[index]; }, nullptr, nullptr,
      false);
  // Only Back is available here, so leave the second slot empty rather than
  // labelling two different buttons the same.
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
