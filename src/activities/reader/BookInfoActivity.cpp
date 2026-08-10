#include "BookInfoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

void BookInfoActivity::onEnter() {
  Activity::onEnter();
  inputGuard_.reset();
  requestUpdate();
}

void BookInfoActivity::loop() {
  if (!inputGuard_.allowsInput(mappedInput,
                               {MappedInputManager::Button::Back, MappedInputManager::Button::Confirm})) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void BookInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_BOOK_INFO));

  std::string format;
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    format = path.substr(dot + 1);
    std::transform(format.begin(), format.end(), format.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  }

  char pageValue[32];
  snprintf(pageValue, sizeof(pageValue), "%d / %d", currentPage, totalPages);
  char progressValue[16];
  snprintf(progressValue, sizeof(progressValue), "%d%%", progressPercent);
  const std::array<const char*, 6> labels = {tr(STR_TITLE),    tr(STR_AUTHOR), tr(STR_FORMAT),
                                             tr(STR_LANGUAGE), tr(STR_PAGE),   tr(STR_PROGRESS)};
  const std::array<std::string, 6> values = {title,     author.empty() ? tr(STR_NOT_SET) : author,
                                             format,    language.empty() ? tr(STR_NOT_SET) : language,
                                             pageValue, progressValue};

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = UITheme::getListContentBottom(renderer, false);
  GUI.drawList(
      renderer, Rect{0, top, width, std::max(0, contentBottom - top)}, static_cast<int>(labels.size()), -1,
      [&labels](const int index) { return std::string(labels[index]); }, nullptr, nullptr,
      [&values](const int index) { return values[index]; }, false);

  // The path is the one value the user opens this screen to read, and it is
  // far wider than the list's value lane — draw it full-width under the list,
  // wrapped, instead of amputating it to ~13 characters.
  int pathY = top + static_cast<int>(labels.size()) * metrics.listRowHeight + metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, pathY, tr(STR_FILE_PATH), true, EpdFontFamily::BOLD);
  pathY += renderer.getLineHeight(UI_10_FONT_ID);
  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int maxPathLines = std::max(1, (contentBottom - pathY) / pathLineHeight);
  for (const auto& line :
       renderer.wrappedText(SMALL_FONT_ID, path.c_str(), width - metrics.contentSidePadding * 2, maxPathLines)) {
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, line.c_str());
    pathY += pathLineHeight;
  }

  // Only Back does anything here, so the second slot stays empty rather than
  // naming two different buttons the same thing.
  const auto buttonLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, buttonLabels.btn1, buttonLabels.btn2, buttonLabels.btn3, buttonLabels.btn4);
  renderer.displayBuffer();
}
