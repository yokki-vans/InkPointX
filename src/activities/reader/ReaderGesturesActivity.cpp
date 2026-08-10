#include "ReaderGesturesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

struct GestureRow {
  StrId action;
  std::string gesture;
};

// Render a hold duration the same way everywhere: "<button> 0.4s". Digits and 's'
// are ASCII, so this needs no glyph beyond what every locale subset already has.
std::string holdNotation(const char* buttonLabel, const unsigned long ms) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%s %.1fs", buttonLabel, static_cast<double>(ms) / 1000.0);
  return buffer;
}

}  // namespace

void ReaderGesturesActivity::onEnter() {
  Activity::onEnter();
  inputGuard_.reset();
  selectorIndex = 0;
  requestUpdate();
}

void ReaderGesturesActivity::loop() {
  if (!inputGuard_.allowsInput(mappedInput, {MappedInputManager::Button::Back, MappedInputManager::Button::Confirm})) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void ReaderGesturesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  // Describe the buttons by the labels the legend would use, so a remapped device
  // documents itself correctly rather than naming the factory layout.
  const auto keys = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  const std::string turnPage = std::string(keys.btn3) + " / " + keys.btn4;

  std::vector<GestureRow> rows;
  rows.reserve(6);
  // The menu gesture is deliberately absent: opening the menu is how the reader
  // reached this screen, so it needs no documenting. What belongs here is what
  // cannot be found by pressing things.
  rows.push_back({StrId::STR_PAGE, turnPage});
  if (SETTINGS.longPressMenuFunction == CrossPointSettings::LP_MENU_BOOKMARK) {
    rows.push_back({StrId::STR_TOGGLE_BOOKMARK, holdNotation(keys.btn2, ReaderUtils::BOOKMARK_HOLD_MS)});
  } else if (SETTINGS.longPressMenuFunction == CrossPointSettings::LP_MENU_DICTIONARY) {
    rows.push_back({StrId::STR_DICTIONARY, holdNotation(keys.btn2, ReaderUtils::GO_HOME_MS)});
  }
  rows.push_back({StrId::STR_HOME, keys.btn1});
  rows.push_back({StrId::STR_OPEN_FROM_FILE, holdNotation(keys.btn1, ReaderUtils::GO_HOME_MS)});
  if (SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP) {
    rows.push_back({StrId::STR_SELECT_CHAPTER, holdNotation(keys.btn4, ReaderUtils::SKIP_HOLD_MS)});
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_CONTROLS));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(0, UITheme::getListContentBottom(renderer, false) - contentTop);

  // selectedIndex -1: this is a reference sheet, nothing here is actionable, so no
  // row should look selected.
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(rows.size()), -1,
      [&rows](const int index) { return std::string(I18N.get(rows[index].action)); }, nullptr, nullptr,
      [&rows](const int index) { return rows[index].gesture; }, false);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
