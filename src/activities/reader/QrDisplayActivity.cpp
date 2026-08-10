#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  inputGuard_.reset();
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  if (!inputGuard_.allowsInput(mappedInput,
                               {MappedInputManager::Button::Back, MappedInputManager::Button::Confirm})) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DISPLAY_QR), nullptr);

  const int availableWidth = pageWidth - 40;
  const int availableHeight = pageHeight - metrics.topPadding - metrics.headerHeight - metrics.verticalSpacing * 2 - 40;
  const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const Rect qrBounds(20, startY, availableWidth, availableHeight);
  bool wasTruncated = false;
  if (!QrUtils::drawQrCode(renderer, qrBounds, textPayload, &wasTruncated)) {
    // The encoder refused the payload — a header over blank space read as a
    // hang, so say what happened.
    GUI.drawEmptyState(renderer, qrBounds, tr(STR_ERROR_GENERAL_FAILURE));
  } else if (wasTruncated) {
    // Only a prefix fit into the code: warn, or a scanned page passes for the
    // whole chapter.
    UITheme::drawCenteredText(
        renderer, Rect{0, 0, pageWidth, pageHeight}, SMALL_FONT_ID,
        pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - renderer.getLineHeight(SMALL_FONT_ID),
        tr(STR_QR_TRUNCATED), true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
