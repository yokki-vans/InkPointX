#include "ActivityManager.h"

#include <FontCacheManager.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "Activity.h"
#include "OpdsServerStore.h"
#include "achievements/AchievementSystem.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/UITheme.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/GalleryActivity.h"
#include "home/HomeActivity.h"
#include "home/LibraryActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/NetworkModeSelectionActivity.h"
#include "reader/ReaderActivity.h"
#include "reader/ReadingStatsActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/AchievementUnlockActivity.h"
#include "util/BootDiag.h"
#include "util/FullScreenMessageActivity.h"

void ActivityManager::begin() {
  const BaseType_t created = xTaskCreate(&renderTaskTrampoline, "ActivityManagerRender",
                                         8192,              // Stack size
                                         this,              // Parameters
                                         1,                 // Priority
                                         &renderTaskHandle  // Task handle
  );
  if (created != pdPASS || renderTaskHandle == nullptr) {
    // Rendering synchronously is slower, but it leaves the reader usable and
    // avoids turning a transient boot-time allocation failure into an abort.
    renderTaskHandle = nullptr;
    LOG_ERR("ACT", "Render task creation failed; using synchronous rendering");
  }
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t generation;
    taskENTER_CRITICAL(&renderStateMux);
    generation = requestedRenderGeneration;
    taskEXIT_CRITICAL(&renderStateMux);
    renderOnce(generation);
  }
}

void ActivityManager::renderOnce(const uint32_t generation) {
  // Acquire the lock before reading currentActivity to avoid a TOCTOU race
  // where the main task deletes the activity between the null-check and render().
  RenderLock lock;
  if (currentActivity) {
    HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
    renderer.beginFrame();
#if LOG_LEVEL >= 2
    auto* fontCache = renderer.getFontCacheManager();
    if (fontCache) fontCache->resetStats();
    const std::string activityName = currentActivity->name;
    const unsigned long renderStartedAt = millis();
    currentActivity->render(std::move(lock));
    LOG_DBG("ACT", "Rendered %s in %lu ms", activityName.c_str(), millis() - renderStartedAt);
    if (fontCache) fontCache->logStats(activityName.c_str());
#else
    currentActivity->render(std::move(lock));
#endif
  }

  // Notify a waiter only when its own generation (not merely any older frame)
  // has completed.
  TaskHandle_t waiter = nullptr;
  taskENTER_CRITICAL(&renderStateMux);
  if (generation > completedRenderGeneration) completedRenderGeneration = generation;
  if (waitingTaskHandle && completedRenderGeneration >= waitingRenderGeneration) {
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    waitingRenderGeneration = 0;
  }
  taskEXIT_CRITICAL(&renderStateMux);
  if (waiter) {
    xTaskNotifyGive(waiter);
  }
}

uint32_t ActivityManager::queueRender() {
  taskENTER_CRITICAL(&renderStateMux);
  const uint32_t generation = ++requestedRenderGeneration;
  taskEXIT_CRITICAL(&renderStateMux);

  if (renderTaskHandle) {
    xTaskNotifyGive(renderTaskHandle);
  } else {
    renderOnce(generation);
  }
  return generation;
}

void ActivityManager::loop() {
  if (currentActivity) {
    // Activity fields are consumed by the render task.  Serializing loop()
    // with render() gives every activity a coherent frame without relying on
    // dozens of individual fields being made atomic.  The mutex is recursive
    // because existing activities also use short RenderLock scopes around
    // callbacks and compound updates.
    // Never park loopTask behind a long page layout/display waveform. Besides
    // preserving power/input housekeeping, this lets the watchdog distinguish
    // a genuinely wedged main task from a render that is legitimately busy.
    RenderLock stateLock(0);
    if (!stateLock.locked()) return;

    // Replay every directional edge captured while the panel was busy before
    // scheduling the next frame. This makes rapid list navigation catch up in
    // one paint instead of losing clicks or paying one ~500 ms waveform per
    // queued step. Screen-changing/system buttons are intentionally limited to
    // one event here so they cannot spill into the activity they launch.
    uint8_t processedEvents = 0;
    constexpr uint8_t MAX_COALESCED_NAV_EVENTS = 12;
    do {
      const bool hadEvent = mappedInput.hasPendingInputEvent();
      currentActivity->loop();
      if (!hadEvent) break;
      mappedInput.consumeInputEvent();
      ++processedEvents;
      if (pendingAction != PendingAction::None || processedEvents >= MAX_COALESCED_NAV_EVENTS ||
          !mappedInput.pendingInputIsNavigationOnly()) {
        break;
      }
    } while (true);
#if LOG_LEVEL >= 2
    if (processedEvents > 1) LOG_DBG("INPUT", "Coalesced %u navigation events into one frame", processedEvents);
#endif
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        prepareDisplayForActivity(*currentActivity);
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        if (stackActivities.size() >= MAX_ACTIVITY_STACK) {
          LOG_ERR("ACT", "Activity stack limit reached; refusing nested activity");
          pendingActivity.reset();
          pendingAction = PendingAction::None;
          continue;
        }
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);
      prepareDisplayForActivity(*currentActivity);

      if (currentActivity->isReaderActivity()) {
        // UI glyphs are cached across menu redraws for responsiveness. Release
        // that bounded cache before book parsing/layout, where heap headroom is
        // more important and the reader has its own per-page font prewarm.
        // Inside the lock: freeing decompressed glyph data while the render
        // task is blitting one is a use-after-free, and a render notification
        // queued before the switch can start the moment the lock drops.
        if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
      }
      // Keep lifecycle initialization serialized with the render task.  The
      // mutex is recursive, so activities may retain their existing short
      // RenderLock scopes and requestUpdateAndWait() temporarily releases the
      // manager-owned level when a physical frame is required.
      currentActivity->onEnter();
      lock.unlock();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  showPendingAchievement();

  bool shouldRender = false;
  taskENTER_CRITICAL(&renderStateMux);
  if (requestedUpdate) {
    requestedUpdate = false;
    shouldRender = true;
  }
  taskEXIT_CRITICAL(&renderStateMux);
  if (shouldRender) queueRender();
}

void ActivityManager::showPendingAchievement() {
  if (!currentActivity || pendingAction != PendingAction::None) return;
  const std::string& activityName = currentActivity->name;
  if (activityName == "AchievementUnlock" || activityName == "Boot" || activityName == "Sleep" ||
      activityName == "Crash" || activityName == "OtaUpdate" || activityName == "SdFirmwareUpdate" ||
      activityName == "FullScreenMessage") {
    return;
  }

  const uint32_t pending = ACHIEVEMENTS.takePendingUnlocks();
  if (pending == 0) return;
  AchievementId first = AchievementId::FirstPage;
  for (size_t i = 0; i < achievementCount(); ++i) {
    if ((pending & achievementBit(static_cast<AchievementId>(i))) != 0) {
      first = static_cast<AchievementId>(i);
      break;
    }
  }

  std::string message = tr(STR_ACHIEVEMENT_UNLOCKED);
  message += "\n";
  message += ACHIEVEMENTS.name(first);
  const uint8_t count = achievementPopcount(pending);
  if (count > 1) {
    message += "\n+";
    message += std::to_string(count - 1);
  }
  pushActivity(makeUniqueNoThrow<AchievementUnlockActivity>(renderer, mappedInput, std::move(message)));
}

void ActivityManager::prepareDisplayForActivity(const Activity& activity) {
  // Every screen the user reaches passes through here, so this is where the
  // post-mortem marker learns what was on screen. BootDiag coalesces and
  // flushes the marker later from the main loop, outside this transition.
  BootDiag::noteScreen(activity.name.c_str());
  const bool reader = activity.isReaderActivity();
  renderer.beginFrame();
  renderer.setFrameOverlayEnabled(!reader);
  // Keep ordinary navigation on the panel's fast partial waveform. Automatic
  // periodic CLEAN refreshes caused a conspicuous full-screen flash and a long
  // pause every ninth menu action on X3/X4. Clean/full refreshes remain
  // available through explicit transition, wake and manual-refresh requests;
  // readers retain their own user-configurable page cleanup cadence.
  renderer.setAutomaticCleanupEnabled(false);
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  if (!newActivity) {
    LOG_ERR("ACT", "Cannot replace activity: allocation failed");
    return;
  }
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    RenderLock lock;
    currentActivity = std::move(newActivity);
    prepareDisplayForActivity(*currentActivity);
    if (currentActivity->isReaderActivity()) {
      if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
    }
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(makeUniqueNoThrow<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToFileTransfer(const NetworkMode mode) {
  replaceActivity(makeUniqueNoThrow<CrossPointWebServerActivity>(renderer, mappedInput, mode));
}

void ActivityManager::goToSettings(const int categoryIndex) {
  replaceActivity(makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, categoryIndex));
}

void ActivityManager::goToLibrary() {
  replaceActivity(makeUniqueNoThrow<LibraryActivity>(renderer, mappedInput, LibraryActivity::Mode::AllBooks));
}

void ActivityManager::goToFavorites() {
  replaceActivity(makeUniqueNoThrow<LibraryActivity>(renderer, mappedInput, LibraryActivity::Mode::Favorites));
}

void ActivityManager::goToReadingStats() {
  replaceActivity(makeUniqueNoThrow<ReadingStatsActivity>(renderer, mappedInput));
}

void ActivityManager::goToGallery() { replaceActivity(makeUniqueNoThrow<GalleryActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(makeUniqueNoThrow<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(makeUniqueNoThrow<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(makeUniqueNoThrow<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(makeUniqueNoThrow<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh) {
  HomeActivity::invalidateDetailsCache();
  replaceActivity(makeUniqueNoThrow<ReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(makeUniqueNoThrow<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(makeUniqueNoThrow<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(makeUniqueNoThrow<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "Library") {
      initialMenuItem = HomeMenuItem::LIBRARY;
    } else if (activityName == "Favorites") {
      initialMenuItem = HomeMenuItem::FAVORITES;
    } else if (activityName == "ReadingStats") {
      initialMenuItem = HomeMenuItem::READING_STATS;
    } else if (activityName == "Gallery") {
      initialMenuItem = HomeMenuItem::GALLERY;
    } else if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(makeUniqueNoThrow<HomeActivity>(renderer, mappedInput, initialMenuItem));
}
void ActivityManager::goToCrashReport() { replaceActivity(makeUniqueNoThrow<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (!activity) {
    LOG_ERR("ACT", "Cannot push activity: allocation failed");
    return;
  }
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const {
  // Consider the whole stack, like isReaderActivity below: an activity that must
  // not be slept through (a transfer, an update, hands-free page turning) keeps
  // that requirement while one of its own dialogs is on top.
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->preventAutoSleep(); }) ||
         (currentActivity && currentActivity->preventAutoSleep());
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

bool ActivityManager::hasCompletedFrame() {
  taskENTER_CRITICAL(&renderStateMux);
  const bool completed = completedRenderGeneration != 0;
  taskEXIT_CRITICAL(&renderStateMux);
  return completed;
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    queueRender();
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    taskENTER_CRITICAL(&renderStateMux);
    requestedUpdate = true;
    taskEXIT_CRITICAL(&renderStateMux);
  }
}
void ActivityManager::requestUpdateAndWait() {
  const TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  const bool isRenderTask = (currentTask == renderTaskHandle);
  const bool holdingRenderLock = (xSemaphoreGetMutexHolder(renderingMutex) == currentTask);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");
  if (isRenderTask) return;

  // ActivityManager::loop() owns one recursive state lock. Release that one
  // level while the display task renders the state just produced, then restore
  // it before returning to the activity. Explicit inner RenderLocks must have
  // ended before calling this method (all current call sites do so).
  if (holdingRenderLock) xSemaphoreGiveRecursive(renderingMutex);

  if (!renderTaskHandle) {
    queueRender();
    if (holdingRenderLock) xSemaphoreTakeRecursive(renderingMutex, portMAX_DELAY);
    return;
  }

  uint32_t generation = 0;
  while (generation == 0) {
    taskENTER_CRITICAL(&renderStateMux);
    if (waitingTaskHandle == nullptr) {
      generation = ++requestedRenderGeneration;
      waitingTaskHandle = currentTask;
      waitingRenderGeneration = generation;
      // This physical frame includes any update deferred earlier in the same
      // activity tick, so do not schedule a redundant second refresh.
      requestedUpdate = false;
    }
    taskEXIT_CRITICAL(&renderStateMux);
    if (generation == 0) vTaskDelay(pdMS_TO_TICKS(1));
  }

  xTaskNotifyGive(renderTaskHandle);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    taskENTER_CRITICAL(&renderStateMux);
    const bool completed = completedRenderGeneration >= generation;
    taskEXIT_CRITICAL(&renderStateMux);
    if (completed) break;
  }

  if (holdingRenderLock) xSemaphoreTakeRecursive(renderingMutex, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTakeRecursive(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock(const TickType_t timeoutTicks) {
  isLocked = xSemaphoreTakeRecursive(activityManager.renderingMutex, timeoutTicks) == pdTRUE;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTakeRecursive(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGiveRecursive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGiveRecursive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xSemaphoreGetMutexHolder(activityManager.renderingMutex) != nullptr; };

ScopedRenderUnlock::ScopedRenderUnlock() {
  const TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
  if (xSemaphoreGetMutexHolder(activityManager.renderingMutex) == currentTask) {
    wasLocked = xSemaphoreGiveRecursive(activityManager.renderingMutex) == pdTRUE;
  }
}

ScopedRenderUnlock::~ScopedRenderUnlock() {
  if (wasLocked) {
    xSemaphoreTakeRecursive(activityManager.renderingMutex, portMAX_DELAY);
  }
}
