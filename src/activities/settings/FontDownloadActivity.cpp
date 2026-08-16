#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "achievements/AchievementSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr const char* FONT_MANIFEST_CACHE = "/fonts_manifest.tmp";
}

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  Storage.remove(FONT_MANIFEST_CACHE);

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // Download manifest to a temp file on SD card to avoid holding both
  // TLS buffers and the full JSON string in RAM simultaneously.
  Storage.remove(FONT_MANIFEST_CACHE);
  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, FONT_MANIFEST_CACHE, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    lastFailedOp_ = FailedOp::Manifest;
    Storage.remove(FONT_MANIFEST_CACHE);
    return false;
  }

  // HTTP client is now closed — TLS buffers freed. Parse JSON from file.
  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", FONT_MANIFEST_CACHE, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(FONT_MANIFEST_CACHE);
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    lastFailedOp_ = FailedOp::Manifest;
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();

  if (err) {
    LOG_ERR("FONT", "Manifest parse error: %s", err.c_str());
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    lastFailedOp_ = FailedOp::Manifest;
    Storage.remove(FONT_MANIFEST_CACHE);
    return false;
  }

  int version = doc["version"] | 0;
  if (version != FONTS_MANIFEST_VERSION) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    lastFailedOp_ = FailedOp::Manifest;
    Storage.remove(FONT_MANIFEST_CACHE);
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  families_.clear();
  fontInstaller_.refreshRegistry();

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  families_.reserve(familiesArr.size());

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    family.name = fObj["name"] | "";
    family.description = fObj["description"] | "";
    family.installed = fontInstaller_.isFamilyInstalled(family.name.c_str());

    family.totalSize = 0;
    JsonArray filesArr = fObj["files"].as<JsonArray>();
    family.fileCount = filesArr.size();
    for (JsonObject fileObj : filesArr) {
      const char* fileName = fileObj["name"] | "";
      const size_t fileSize = fileObj["size"] | 0;

      if (!fileObj["crc32"].is<uint32_t>()) {
        LOG_ERR("FONT", "Malformed manifest file entry: missing or invalid crc32 for %s", fileName);
        errorMessage_ = tr(STR_FONT_LIST_FAILED);
        lastFailedOp_ = FailedOp::Manifest;
        Storage.remove(FONT_MANIFEST_CACHE);
        return false;
      }

      family.totalSize += fileSize;

      // Detect updates while the JsonDocument is alive instead of retaining
      // every file name/CRC in RAM for the lifetime of this screen.
      if (family.installed && !family.hasUpdate) {
        char path[128];
        FontInstaller::buildFontPath(family.name.c_str(), fileName, path, sizeof(path));
        HalFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != fileSize) family.hasUpdate = true;
        } else {
          // File missing on disk but family dir exists — treat as update
          family.hasUpdate = true;
        }
      }
    }

    LOG_DBG("FONT", "Catalog family %s: installed=%d update=%d files=%u", family.name.c_str(), family.installed,
            family.hasUpdate, static_cast<unsigned>(family.fileCount));
    families_.push_back(std::move(family));
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

bool FontDownloadActivity::loadFamilyFiles(const int familyIndex, ManifestFamily& outFamily) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return false;

  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", FONT_MANIFEST_CACHE, manifestFile)) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();
  if (err || (doc["version"] | 0) != FONTS_MANIFEST_VERSION) return false;

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  if (familyIndex >= static_cast<int>(familiesArr.size())) return false;
  JsonObject familyObj = familiesArr[familyIndex].as<JsonObject>();

  outFamily = ManifestFamily{};
  outFamily.name = familyObj["name"] | "";
  if (outFamily.name != families_[familyIndex].name) return false;
  outFamily.installed = families_[familyIndex].installed;
  outFamily.hasUpdate = families_[familyIndex].hasUpdate;

  JsonArray filesArr = familyObj["files"].as<JsonArray>();
  outFamily.files.reserve(filesArr.size());
  for (JsonObject fileObj : filesArr) {
    ManifestFile file;
    file.name = fileObj["name"] | "";
    file.size = fileObj["size"] | 0;
    if (file.name.empty() || !fileObj["crc32"].is<uint32_t>()) return false;
    file.crc32 = fileObj["crc32"].as<uint32_t>();
    outFamily.totalSize += file.size;
    outFamily.files.push_back(std::move(file));
  }
  outFamily.fileCount = outFamily.files.size();
  return !outFamily.files.empty();
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed) continue;
    // finalize=false: COMPLETE flashed between families otherwise.
    downloadFamily(static_cast<int>(i), false);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].hasUpdate) continue;
    downloadFamily(static_cast<int>(i), false);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const auto& f : families_) {
    if (!f.installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const auto& f : families_) {
    if (f.hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return families_.empty() ? 0 : static_cast<int>(families_.size()) + specialRowCount();
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed) total += f.totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.hasUpdate) total += f.totalSize;
  }
  return total;
}

void FontDownloadActivity::downloadFamily(const int familyIndex, const bool finalize) {
  ManifestFamily family;
  if (!loadFamilyFiles(familyIndex, family)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_LIST_FAILED);
    lastFailedOp_ = FailedOp::Manifest;
    return;
  }

  auto& catalogFamily = families_[familyIndex];
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = familyIndex;
    downloadingFamilyName_ = family.name;
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  if (!fontInstaller_.ensureFamilyDir(family.name.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_DOWNLOAD_FAILED);
    lastFailedOp_ = FailedOp::Download;
    return;
  }

  // On any failure below: a family that was already installed keeps its
  // working files (only the staging file is discarded); a half-downloaded
  // fresh install is deleted outright. The old code deleted the family in
  // both cases, so a dropped connection during an update destroyed the font
  // the user already had.
  const bool wasInstalled = catalogFamily.installed;
  const auto failCleanup = [&](const char* stagePath) {
    Storage.remove(stagePath);
    if (!wasInstalled) {
      fontInstaller_.deleteFamily(family.name.c_str());
      catalogFamily.installed = false;
      catalogFamily.hasUpdate = false;
    }
  };

  // GitHub release links first redirect to signed CDN URLs. Resolve all four
  // links over one reusable github.com connection: an ESP32-C3 otherwise pays
  // the expensive certificate/ECC handshake once per font size. Failure is a
  // performance concern only; the ordinary downloader remains the fallback.
  std::vector<std::string> downloadUrls;
  downloadUrls.reserve(family.files.size());
  for (const auto& file : family.files) downloadUrls.push_back(baseUrl_ + file.name);
  std::vector<std::string> resolvedUrls;
  if (HttpDownloader::resolveFirstRedirects(downloadUrls, resolvedUrls)) {
    downloadUrls = std::move(resolvedUrls);
  }

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
      lastNotifiedPercent_ = -1;
      lastProgressRenderMs_ = millis();
    }
    requestUpdateAndWait();

    char destPath[128];
    FontInstaller::buildFontPath(family.name.c_str(), file.name.c_str(), destPath, sizeof(destPath));
    // Stage next to the destination and swap only after validation.
    char stagePath[136];
    snprintf(stagePath, sizeof(stagePath), "%s.new", destPath);

    const std::string& url = downloadUrls[i];

    uint32_t actualCrc = 0;
    auto result = HttpDownloader::downloadToFile(
        url, stagePath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          // GitHub's release CDN may use chunked transfer. Preserve the size
          // from fonts.json when HTTP has no Content-Length instead of
          // replacing a valid denominator with zero.
          if (total > 0) fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          const size_t effectiveTotal = fileTotal_;
          const int percent =
              effectiveTotal > 0 ? std::min(100, static_cast<int>(downloaded * 100 / effectiveTotal)) : 0;
          const unsigned long now = millis();
          // A synchronous download runs under ActivityManager's state lock.
          // requestUpdate() alone queues frames that cannot render until the
          // transfer ends, which left the screen at 0%. Release that lock and
          // wait for a real frame at e-ink-friendly intervals: first visible
          // progress, then at most once per 20 percentage points and 1.5 s.
          const bool firstVisibleProgress = lastNotifiedPercent_ < 0 && percent > 0;
          const bool reachedNextStep =
              lastNotifiedPercent_ >= 0 && percent >= lastNotifiedPercent_ + 20 && now - lastProgressRenderMs_ >= 1500;
          if (firstVisibleProgress || reachedNextStep) {
            lastNotifiedPercent_ = percent;
            lastProgressRenderMs_ = now;
            requestUpdateAndWait();
          }
        },
        &cancelRequested_, "", "", &actualCrc);

    if (result == HttpDownloader::ABORTED) {
      failCleanup(stagePath);
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      return;
    }

    const auto failWith = [&](const char* logMsg) {
      LOG_ERR("FONT", "%s: %s", logMsg, file.name.c_str());
      failCleanup(stagePath);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string(tr(STR_FONT_DOWNLOAD_FAILED)) + " (" + file.name + ")";
      lastFailedOp_ = FailedOp::Download;
    };

    if (result != HttpDownloader::OK) {
      failWith("Download failed");
      return;
    }

    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      failWith("Checksum mismatch");
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

    if (!fontInstaller_.validateCpfontFile(stagePath)) {
      failWith("Invalid .cpfont");
      return;
    }

    // Validated: swap into place.
    Storage.remove(destPath);
    if (!Storage.rename(stagePath, destPath)) {
      failWith("Failed to move staged font");
      return;
    }
    currentFileIndex_++;
  }

  fontInstaller_.refreshRegistry();
  catalogFamily.installed = true;
  catalogFamily.hasUpdate = false;
  ACHIEVEMENTS.record(AchievementEvent::FontDownloaded);

  if (finalize) {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(selectedIndex_);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = family.name;
  startActivityForResult(makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  auto& family = families_[familyIndexFromList(selectedIndex_)];

  if (fontInstaller_.deleteFamily(family.name.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_DELETE_FAILED);
    lastFailedOp_ = FailedOp::Delete;
  } else {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(selectedIndex_) || isUpdateAllRow(selectedIndex_)) return false;
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(selectedIndex_)];
  return family.installed && !family.hasUpdate;
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    // hasSubtitle=true: the rows are 86 px, so the old 62 px page count
    // paged three rows past what the screen shows. The extra reserve matches
    // the footer counter render() now draws.
    const int pageItems =
        UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true, BaseTheme::footerCounterTopOffset);

    buttonNavigator_.onNextPress([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onPreviousPress([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        if (isDownloadAllRow(selectedIndex_)) {
          currentFileIndex_ = 0;
          currentFileTotal_ = 0;
          for (const auto& f : families_) {
            if (!f.installed) currentFileTotal_ += f.fileCount;
          }

          downloadAll();
        } else if (isUpdateAllRow(selectedIndex_)) {
          currentFileIndex_ = 0;
          currentFileTotal_ = 0;
          for (const auto& f : families_) {
            if (f.hasUpdate) currentFileTotal_ += f.fileCount;
          }
          updateAll();
        } else {
          const int familyIndex = familyIndexFromList(selectedIndex_);
          auto& family = families_[familyIndex];
          if (!family.installed || family.hasUpdate) {
            currentFileIndex_ = 0;
            currentFileTotal_ = family.fileCount;
            downloadFamily(familyIndex);
          } else {
            promptDeleteSelectedFamily();
            return;
          }
        }
        requestUpdateAndWait();
        return;
      }
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Retry the operation that actually failed. The old code always
      // re-downloaded family 0 (the index default), so Retry after a manifest
      // or delete failure did something unrelated.
      if (lastFailedOp_ == FailedOp::Manifest) {
        onWifiSelectionComplete(true);
        requestUpdate();
        return;
      }
      if (lastFailedOp_ == FailedOp::Download && downloadingFamilyIndex_ >= 0 &&
          downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        currentFileIndex_ = 0;
        currentFileTotal_ = families_[downloadingFamilyIndex_].fileCount;
        downloadFamily(downloadingFamilyIndex_);
        requestUpdateAndWait();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      GUI.drawEmptyState(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - contentTop},
          tr(STR_NO_FONTS_AVAILABLE), nullptr, /*script=*/true);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, std::max(0, UITheme::getListContentBottom(renderer, true) - contentTop)},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index)) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
            }
            if (isUpdateAllRow(index)) {
              return std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            return families_[familyIndexFromList(index)].description;
          },
          nullptr,
          [this](int index) -> std::string {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            return "";
          },
          true,
          [this](int index) -> bool {
            if (isDownloadAllRow(index) || isUpdateAllRow(index)) return false;
            const auto& f = families_[familyIndexFromList(index)];
            return f.installed && !f.hasUpdate;
          });

      GUI.drawFooterCounter(renderer, selectedIndex_, listItemCount());
      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                isSelectedFamilyDeletable()      ? tr(STR_DELETE)
                                                : isUpdateAllRow(selectedIndex_) ? tr(STR_UPDATE)
                                                                                 : tr(STR_DOWNLOAD),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + downloadingFamilyName_ + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    GUI.drawEmptyState(
        renderer,
        Rect{0, contentTop, pageWidth, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - contentTop},
        tr(STR_FONT_INSTALLED), nullptr, /*script=*/true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    const Rect messageArea{0, contentTop, pageWidth,
                           pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - contentTop};
    GUI.drawEmptyState(renderer, messageArea, tr(STR_FONT_INSTALL_FAILED),
                       errorMessage_.empty() ? nullptr : errorMessage_.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
