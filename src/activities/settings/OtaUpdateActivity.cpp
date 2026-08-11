#include "OtaUpdateActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/LayoutGeometry.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"
#include "util/BootDiag.h"

namespace {
constexpr int UPDATE_CARD_RADIUS = 14;
constexpr int UPDATE_CARD_ACCENT_WIDTH = 5;
constexpr int UPDATE_CARD_SIDE_PADDING = 18;
}  // namespace

void OtaUpdateActivity::prepareReleaseNoteLines(const int wrapWidth) {
  if (releaseNotesWrapWidth == wrapWidth && !releaseNoteLines.empty()) return;
  releaseNotesWrapWidth = wrapWidth;
  releaseNoteLines.clear();
  releaseNotesPage = 0;

  const std::string& notes = updater.getReleaseNotes();
  if (notes.empty()) {
    releaseNoteLines.emplace_back(tr(STR_NO_ENTRIES));
    return;
  }

  size_t position = 0;
  while (position <= notes.size()) {
    const size_t end = notes.find('\n', position);
    const std::string paragraph = notes.substr(position, end == std::string::npos ? std::string::npos : end - position);
    if (paragraph.empty()) {
      if (!releaseNoteLines.empty() && !releaseNoteLines.back().empty()) releaseNoteLines.emplace_back();
    } else {
      auto wrapped = renderer.wrappedText(UI_10_FONT_ID, paragraph.c_str(), wrapWidth, 96);
      releaseNoteLines.insert(releaseNoteLines.end(), std::make_move_iterator(wrapped.begin()),
                              std::make_move_iterator(wrapped.end()));
    }
    if (end == std::string::npos) break;
    position = end + 1;
  }
  while (!releaseNoteLines.empty() && releaseNoteLines.back().empty()) releaseNoteLines.pop_back();
  if (releaseNoteLines.empty()) releaseNoteLines.emplace_back(tr(STR_NO_ENTRIES));
}

const char* OtaUpdateActivity::failureText(const int result) {
  // The user could not previously tell "no network" from "bad image" — the
  // reason was logged and thrown away.
  switch (result) {
    case OtaUpdater::HTTP_ERROR:
      return tr(STR_SYNC_ERR_NETWORK);
    case OtaUpdater::OOM_ERROR:
      return tr(STR_MEMORY_ERROR);
    case OtaUpdater::STORAGE_ERROR:
      return tr(STR_SD_CARD_ERROR);
    case OtaUpdater::INVALID_FIRMWARE_ERROR:
      return tr(STR_INVALID_FIRMWARE);
    case OtaUpdater::FLASH_ERROR:
      return tr(STR_FIRMWARE_WRITE_FAILED);
    case OtaUpdater::JSON_PARSE_ERROR:
    case OtaUpdater::UPDATE_OLDER_ERROR:
    case OtaUpdater::INTERNAL_UPDATE_ERROR:
    default:
      return tr(STR_ERROR_GENERAL_FAILURE);
  }
}

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  // The physical e-ink screen retains the checking state. Release decompressed
  // UI glyphs before GitHub TLS allocates its working buffers; cache access is
  // shared with the render task, so it must happen under the render lock.
  {
    RenderLock lock(*this);
    if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
  }

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      state = FAILED;
      failureReason = failureText(res);
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }

  {
    RenderLock lock(*this);
    releaseNoteLines.clear();
    releaseNotesPage = 0;
    releaseNotesWrapWidth = 0;
    state = WAITING_CONFIRMATION;
  }
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Success path reboots via the SHUTTING_DOWN state's plain ESP.restart()
  // (loop() above) so the new firmware boots normally. Back-out paths land
  // here with wifi still active; silent-restart to free the LWIP/mbedTLS
  // fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentSpan = LayoutGeometry::menuContentSpan(pageHeight, metrics.topPadding, metrics.headerHeight,
                                                           metrics.verticalSpacing, metrics.buttonHintsHeight);
  const Rect messageArea{0, contentSpan.y, pageWidth, contentSpan.height};
  const auto top = LayoutGeometry::centeredBlockTop(contentSpan.y, contentSpan.height, height);

  float updaterProgress = 0;
  size_t updateProcessed = 0;
  size_t updateTotal = 0;
  OtaUpdater::Phase updatePhase = OtaUpdater::Phase::IDLE;
  if (state == UPDATE_IN_PROGRESS) {
    updateProcessed = updater.getProcessedSize();
    updateTotal = updater.getTotalSize();
    updatePhase = updater.getPhase();
    LOG_DBG("OTA", "Update progress: %u / %u", static_cast<unsigned>(updateProcessed),
            static_cast<unsigned>(updateTotal));
    if (updateTotal > 0) {
      updaterProgress = static_cast<float>(updateProcessed) / static_cast<float>(updateTotal);
    }
    const int updatePercent = static_cast<int>(updaterProgress * 100);
    if (updatePercent == lastUpdaterPercentage && updatePhase == lastUpdaterPhase) {
      return;
    }
    lastUpdaterPercentage = updatePercent;
    lastUpdaterPhase = updatePhase;
  }

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    const bool compact = pageHeight < 600;
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int cardX = metrics.contentSidePadding;
    const int cardWidth = pageWidth - metrics.contentSidePadding * 2;
    const int versionCardHeight = compact ? 72 : 92;
    renderer.drawRoundedRect(cardX, contentTop, cardWidth, versionCardHeight, 1, UPDATE_CARD_RADIUS, true);
    renderer.fillRoundedRect(cardX + 10, contentTop + 12, UPDATE_CARD_ACCENT_WIDTH, versionCardHeight - 24, 2,
                             Color::Black);

    const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const std::string availableTitle =
        renderer.truncatedText(UI_10_FONT_ID, tr(STR_NEW_UPDATE), cardWidth - 52, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, cardX + 28, contentTop + (compact ? 8 : 12), availableTitle.c_str(), true,
                      EpdFontFamily::BOLD);

    const std::string currentVersion = CROSSPOINT_VERSION;
    const std::string latestVersion = updater.getLatestVersion();
    const int currentWidth = renderer.getTextWidth(UI_10_FONT_ID, currentVersion.c_str());
    const int latestWidth = renderer.getTextWidth(UI_10_FONT_ID, latestVersion.c_str(), EpdFontFamily::BOLD);
    constexpr int arrowWidth = 34;
    const int transitionWidth = currentWidth + arrowWidth + latestWidth;
    const int transitionX = cardX + std::max(28, (cardWidth - transitionWidth) / 2);
    const int transitionY = contentTop + (compact ? 39 : 53);
    renderer.drawText(UI_10_FONT_ID, transitionX, transitionY, currentVersion.c_str());
    const int arrowX = transitionX + currentWidth + 9;
    const int arrowY = transitionY + titleLineHeight / 2;
    renderer.drawLine(arrowX, arrowY, arrowX + 16, arrowY, true);
    renderer.drawLine(arrowX + 12, arrowY - 4, arrowX + 16, arrowY, true);
    renderer.drawLine(arrowX + 12, arrowY + 4, arrowX + 16, arrowY, true);
    renderer.drawText(UI_10_FONT_ID, transitionX + currentWidth + arrowWidth, transitionY, latestVersion.c_str(), true,
                      EpdFontFamily::BOLD);

    const int sectionY = contentTop + versionCardHeight + (compact ? 8 : 14);
    // The built-in handwritten face intentionally omits Arabic and Hebrew to
    // save flash; use the complete structural face for those locales. A user-
    // supplied accent font still remains available on all other scripts.
    const int sectionFont = I18N.isRtl() ? UI_12_FONT_ID : (compact ? SCRIPT_SMALL_FONT_ID : SCRIPT_FONT_ID);
    const int sectionLineHeight = renderer.getLineHeight(sectionFont);
    renderer.drawText(sectionFont, cardX, sectionY, tr(STR_WHATS_NEW));

    const int notesTop = sectionY + sectionLineHeight + (compact ? 3 : 6);
    const int notesBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
    const int notesHeight = std::max(70, notesBottom - notesTop);
    renderer.drawRoundedRect(cardX, notesTop, cardWidth, notesHeight, 1, UPDATE_CARD_RADIUS, true);
    renderer.fillRoundedRect(cardX + 10, notesTop + 12, 3, notesHeight - 24, 1, Color::Black);

    const int notesX = cardX + UPDATE_CARD_SIDE_PADDING + 8;
    const int notesWidth = cardWidth - (UPDATE_CARD_SIDE_PADDING + 8) * 2;
    prepareReleaseNoteLines(notesWidth);
    const int noteLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    releaseNotesLinesPerPage = std::max(1, (notesHeight - 24) / noteLineHeight);
    const int pageCount = std::max(
        1, (static_cast<int>(releaseNoteLines.size()) + releaseNotesLinesPerPage - 1) / releaseNotesLinesPerPage);
    releaseNotesPage = std::clamp(releaseNotesPage, 0, pageCount - 1);
    const int firstLine = releaseNotesPage * releaseNotesLinesPerPage;
    const int lastLine = std::min(static_cast<int>(releaseNoteLines.size()), firstLine + releaseNotesLinesPerPage);
    int lineY = notesTop + 12;
    for (int i = firstLine; i < lastLine; ++i) {
      if (!releaseNoteLines[i].empty()) renderer.drawText(UI_10_FONT_ID, notesX, lineY, releaseNoteLines[i].c_str());
      lineY += noteLineHeight;
    }

    if (pageCount > 1) {
      char counter[32];
      snprintf(counter, sizeof(counter), "%d / %d", releaseNotesPage + 1, pageCount);
      const int counterWidth = renderer.getTextWidth(SMALL_FONT_ID, counter);
      renderer.drawText(SMALL_FONT_ID, cardX + cardWidth - counterWidth, sectionY + 3, counter);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), pageCount > 1 ? tr(STR_DIR_UP) : "",
                                              pageCount > 1 ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    const char* phaseText = tr(STR_UPDATING);
    if (updatePhase == OtaUpdater::Phase::DOWNLOADING) {
      phaseText = tr(STR_DOWNLOADING);
    } else if (updatePhase == OtaUpdater::Phase::VERIFYING) {
      phaseText = tr(STR_VALIDATING_FIRMWARE);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, top, phaseText);

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the bytes line below stays at the same Y it was at when the activity drew its own percent.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y,
                              (std::to_string(updateProcessed) + " / " + std::to_string(updateTotal)).c_str());
    // Same warning the SD flasher shows: an interrupted OTA is a brick risk.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF), true, EpdFontFamily::BOLD);
  } else if (state == NO_UPDATE) {
    GUI.drawEmptyState(renderer, messageArea, tr(STR_NO_UPDATE), nullptr, /*script=*/true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    GUI.drawEmptyState(
        renderer,
        Rect{0, contentTop, pageWidth, pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - contentTop},
        tr(STR_UPDATE_FAILED), failureReason);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    GUI.drawEmptyState(renderer, messageArea, tr(STR_UPDATE_COMPLETE), tr(STR_POWER_ON_HINT), /*script=*/true);
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    const int pageCount = std::max(
        1, (static_cast<int>(releaseNoteLines.size()) + releaseNotesLinesPerPage - 1) / releaseNotesLinesPerPage);
    if ((mappedInput.wasPressed(MappedInputManager::Button::Up) ||
         mappedInput.wasPressed(MappedInputManager::Button::Left)) &&
        releaseNotesPage > 0) {
      --releaseNotesPage;
      requestUpdate();
      return;
    }
    if ((mappedInput.wasPressed(MappedInputManager::Button::Down) ||
         mappedInput.wasPressed(MappedInputManager::Button::Right)) &&
        releaseNotesPage + 1 < pageCount) {
      ++releaseNotesPage;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
      }
      requestUpdateAndWait();

      // Keep the already-rendered update screen on the panel while freeing
      // font caches and changelog layout that would otherwise compete with
      // TLS for contiguous heap. E-ink retains the progress frame without any
      // of these display-only allocations remaining alive.
      {
        RenderLock lock(*this);
        std::vector<std::string>().swap(releaseNoteLines);
        updater.discardReleaseNotes();
        if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
      }
      OtaUpdater::OtaUpdaterError res;
      {
        // ActivityManager::loop() normally holds the render mutex while an
        // activity's loop() runs. OTA is synchronous, so keeping that mutex
        // here would block every queued progress frame until installation had
        // already finished. Release exactly the loop-owned recursive level and
        // restore it before changing activity state below.
        ScopedRenderUnlock allowProgressFrames;
        res = updater.installUpdate(
            [](void* ctx) {
              // immediate=true notifies the render task directly. The default deferred path only
              // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
              // installUpdate() blocks this task.
              static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
            },
            this);
      }

      if (res != OtaUpdater::OK) {
        LOG_DBG("OTA", "Update failed: %d", res);
        {
          RenderLock lock(*this);
          state = FAILED;
          failureReason = failureText(res);
        }
        requestUpdate();
        return;
      }

      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      requestUpdateAndWait();
      // Hold the completion screen briefly so the user sees it, then restart.
      delay(3000);
      {
        RenderLock lock(*this);
        state = SHUTTING_DOWN;
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Re-run the whole check: covers both a failed check and a failed
      // install without special-casing.
      if (WiFi.status() == WL_CONNECTED) {
        onWifiSelectionComplete(true);
        requestUpdate();
      } else {
        startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                               [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
      }
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    BootDiag::markCleanShutdown(BootDiag::Shutdown::Restart);
    ESP.restart();
  }
}
