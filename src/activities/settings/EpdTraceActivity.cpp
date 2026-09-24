#include "EpdTraceActivity.h"

#if defined(INKPOINTX_DEVICE_QA)

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/EpdTrace.h"

namespace {
constexpr char kLogPath[] = "/.crosspoint/epd.log";
constexpr size_t kMaxBytes = 8192;
constexpr int kHugeLineCount = 999;
}  // namespace

void EpdTraceActivity::onEnter() {
  Activity::onEnter();
  displayLines.clear();
  scrollOffset = 0;

  char buf[kMaxBytes + 1];
  size_t len = 0;
  HalFile file;
  if (Storage.openFileForRead("EPDTRACE", kLogPath, file)) {
    while (len < kMaxBytes) {
      const int n = file.read(buf + len, kMaxBytes - len);
      if (n <= 0) break;
      len += static_cast<size_t>(n);
    }
    file.close();
  }
  if (len == 0) len = EpdTrace::snapshot(buf, kMaxBytes);
  buf[len] = '\0';

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentWidth = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  std::string text(buf, len);
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    const std::string row = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? text.size() : nl + 1;
    if (row.empty()) continue;
    auto wrapped = renderer.wrappedText(UI_10_FONT_ID, row.c_str(), contentWidth, kHugeLineCount);
    for (auto& w : wrapped) displayLines.push_back(std::move(w));
  }
  if (displayLines.empty()) {
    displayLines.push_back("(no trace lines yet — open a menu, press a few keys,");
    displayLines.push_back(" then come back here)");
  }

  // Visible page: leave comfortable slack above the button-hint row so the
  // last page never draws under it (under-estimating only wastes one scroll).
  const int usableTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int usableBottomSlack = 4 * lineHeight + 3 * metrics.verticalSpacing;
  const int usable = renderer.getScreenHeight() - usableTop - usableBottomSlack;
  pageSize = usable > lineHeight ? usable / lineHeight : 1;

  requestUpdateAndWait();
}

void EpdTraceActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  const int maxOffset =
      displayLines.size() > static_cast<size_t>(pageSize) ? static_cast<int>(displayLines.size()) - pageSize : 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    scrollOffset -= pageSize;
    if (scrollOffset < 0) scrollOffset = 0;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    scrollOffset += pageSize;
    if (scrollOffset > maxOffset) scrollOffset = maxOffset;
    requestUpdate();
  }
}

void EpdTraceActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_EPD_TRACE_VIEW));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  char meta[128];
  const int total = static_cast<int>(displayLines.size());
  const int first = displayLines.empty() ? 0 : scrollOffset + 1;
  const int last = std::min(total, scrollOffset + pageSize);
  snprintf(meta, sizeof(meta), "lines %d-%d / %d  %s", first, last, total, kLogPath);
  renderer.drawText(UI_10_FONT_ID, x, y, meta);
  y += lineHeight + metrics.verticalSpacing;

  const int end = std::min(total, scrollOffset + pageSize);
  for (int i = scrollOffset; i < end; ++i) {
    if (y + lineHeight > renderer.getScreenHeight() - (lineHeight + metrics.verticalSpacing)) break;
    renderer.drawText(UI_10_FONT_ID, x, y, displayLines[static_cast<size_t>(i)].c_str());
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // INKPOINTX_DEVICE_QA
