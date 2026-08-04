#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

// Global HalDisplay instance
HalDisplay display;

HalDisplay::HalDisplay()
    : einkDisplay(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi, BoardConfig::ACTIVE.display.cs,
                  BoardConfig::ACTIVE.display.dc, BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy) {}

HalDisplay::~HalDisplay() {}

bool HalDisplay::begin(bool seamless) {
  einkDisplay.begin();
  ready = einkDisplay.getFrameBuffer() != nullptr;
  if (!ready) return false;
  refreshPolicy.reset();

  if (seamless) {
    // Keep the retained panel frame until the first X4 Pro paint.
    einkDisplay.skipInitialResync();
    return true;
  }
  // The X4 Pro driver invalidates its differential baseline in begin() and
  // promotes the first FAST frame to a clean update itself.
  return true;
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  mode = applyRefreshPolicy(mode);
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  mode = applyRefreshPolicy(mode);
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  fallback = applyRefreshPolicy(fallback);
  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

HalDisplay::RefreshMode HalDisplay::applyRefreshPolicy(const RefreshMode requested) {
  const auto policyMode = requested == FULL_REFRESH   ? EInkRefreshPolicy::Mode::Full
                          : requested == HALF_REFRESH ? EInkRefreshPolicy::Mode::Clean
                                                      : EInkRefreshPolicy::Mode::Fast;
  switch (refreshPolicy.consume(policyMode)) {
    case EInkRefreshPolicy::Mode::Full:
      return FULL_REFRESH;
    case EInkRefreshPolicy::Mode::Clean:
      return HALF_REFRESH;
    case EInkRefreshPolicy::Mode::Fast:
    default:
      return FAST_REFRESH;
  }
}

void HalDisplay::requestCleanRefresh() { refreshPolicy.requestClean(); }

void HalDisplay::requestFullRefresh() {
  refreshPolicy.requestFull();
  einkDisplay.requestResync();
}

void HalDisplay::setAutomaticCleanupEnabled(const bool enabled) { refreshPolicy.setAutomaticCleanupEnabled(enabled); }

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
