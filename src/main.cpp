#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FavoriteBooksStore.h"
#include "InterfaceFont.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/reader/ProgressFile.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/OtaUpdateActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/BootDiag.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

// Native PDF parsing has legitimate recursive dictionary/array paths and can
// enter newlib formatting while several parser frames are live.  The Arduino
// default (8 KiB) is too small for real-world PDFs on ESP32-C3.
SET_LOOP_TASK_STACK_SIZE(16384);

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
static bool bootCoreInitialized = false;
static bool displayInitFailed = false;
// A wake press is not application input. Keep setup non-blocking while the
// user is still holding Power, then discard that release edge in loop().
static bool bootPowerHeld = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

// Inter's Medium and SemiBold weights, instanced from its variable axes and
// rasterized to compact 1-bit translation subsets. Medium keeps body labels open on
// the X4 panel; SemiBold supplies headings and selected/emphasized states. Hebrew,
// Arabic and Korean code points come from Noto fallbacks in the same
// subsets -- see scripts/build_ui_fonts.py. Korean uses Medium for emphasis as
// well, avoiding duplicate Hangul bitmaps in the OTA-constrained image.
EpdFont ui8MediumFont(&ui_8_medium);
EpdFont ui8SemiBoldFont(&ui_8_semibold);
EpdFontFamily ui8FontFamily(&ui8MediumFont, &ui8SemiBoldFont);
EpdFontFamily ui8KoreanFontFamily(&ui8MediumFont);

EpdFont ui10MediumFont(&ui_10_medium);
EpdFont ui10SemiBoldFont(&ui_10_semibold);
EpdFontFamily ui10FontFamily(&ui10MediumFont, &ui10SemiBoldFont);
EpdFontFamily ui10KoreanFontFamily(&ui10MediumFont);
// Handwritten accent voice (Caveat 600). The smaller cut keeps the Home author
// subordinate to the title; both are single-face families because the script
// itself supplies the emphasis.
EpdFont uiScriptSmallFont(&ui_script_16);
EpdFontFamily uiScriptSmallFontFamily(&uiScriptSmallFont);
EpdFont uiScriptFont(&ui_script_20);
EpdFontFamily uiScriptFontFamily(&uiScriptFont);

EpdFont ui12MediumFont(&ui_12_medium);
EpdFont ui12SemiBoldFont(&ui_12_semibold);
EpdFontFamily ui12FontFamily(&ui12MediumFont, &ui12SemiBoldFont);
EpdFontFamily ui12KoreanFontFamily(&ui12MediumFont);

EpdFont ui14MediumFont(&ui_14_medium);
EpdFont ui14SemiBoldFont(&ui_14_semibold);
EpdFontFamily ui14FontFamily(&ui14MediumFont, &ui14SemiBoldFont);
EpdFontFamily ui14KoreanFontFamily(&ui14MediumFont);

EpdFont ui16MediumFont(&ui_16_medium);
EpdFont ui16SemiBoldFont(&ui_16_semibold);
EpdFontFamily ui16FontFamily(&ui16MediumFont, &ui16SemiBoldFont);
EpdFontFamily ui16KoreanFontFamily(&ui16MediumFont);

// Screen headings sit at the top of the scale.
EpdFontFamily uiHeaderFontFamily(&ui16SemiBoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void drawSystemFrameOverlay(const GfxRenderer& target) { UITheme::getInstance().drawSystemBatteryOverlay(target); }

void applyInterfaceFont() {
  // Swapping the interface faces unregisters seven font IDs and frees the
  // decompressed glyph cache. The render task reads both while it draws, and
  // the two settings screens that change the face call this straight from
  // their loop() on the main task — so without the lock the map is mutated
  // mid-lookup and the cache is freed under a glyph that is being blitted.
  // Held for the whole swap: a half-applied font map is not a valid state to
  // render from either. begin() creates the mutex before setup() reaches the
  // first call here, so this is also safe during boot.
  RenderLock lock;
  renderer.removeFont(MICRO_FONT_ID);
  renderer.removeFont(SMALL_FONT_ID);
  renderer.removeFont(UI_10_FONT_ID);
  renderer.removeFont(UI_12_FONT_ID);
  renderer.removeFont(UI_14_FONT_ID);
  renderer.removeFont(UI_16_FONT_ID);
  renderer.removeFont(UI_18_FONT_ID);
  renderer.removeFont(HEADER_FONT_ID);
  renderer.removeFont(SCRIPT_SMALL_FONT_ID);
  renderer.removeFont(SCRIPT_FONT_ID);
  if (renderer.getFontCacheManager()) renderer.getFontCacheManager()->clearCache();

  // The UI_nn identifiers are slot names, not pixel sizes. The whole scale sits one
  // step higher than the slot names suggest, so the interface reads comfortably at
  // arm's length on a 480 x 800 panel rather than merely fitting on it. Row heights,
  // the header, the legend bar and the footer counter are sized from these line
  // heights in LyraMetrics, so the two move together.
  //
  // MICRO exists for one job: the keyboard's secondary key labels. That grid is
  // structurally dense (10 columns of single characters) and does not benefit from
  // larger type, so it keeps the size the rest of the interface has outgrown.
  // The whole scale sits one step below the previous build (user request):
  // captions 12->10, labels 14->12, row titles 16->14, headings and book
  // titles 18->16. MICRO keeps 8. The 18 pt family is no longer linked.
  // A family on the card can stand in for the whole scale. Each slot takes the
  // closest size it ships, and every slot it cannot cover keeps the built-in
  // face — a card font with only two sizes is still usable, and a card that
  // was pulled out falls back to a complete interface rather than a blank one.
  static constexpr SdCardFontSystem::InterfaceSlot uiSlots[] = {
      {MICRO_FONT_ID, 8, 2},  {SMALL_FONT_ID, 10, 2}, {UI_10_FONT_ID, 12, 2}, {UI_12_FONT_ID, 14, 2},
      {UI_14_FONT_ID, 16, 2}, {UI_16_FONT_ID, 16, 2}, {UI_18_FONT_ID, 16, 2},
  };
  static constexpr SdCardFontSystem::InterfaceSlot accentSlots[] = {
      {SCRIPT_SMALL_FONT_ID, 16, 6},
      {SCRIPT_FONT_ID, 20, 6},
  };

  sdFontSystem.unloadInterfaceFaces(renderer);
  const bool korean = I18N.getLanguage() == Language::KO;
  // The catalog's optional interface families do not promise Hangul coverage.
  // Preserve the user's selection, but use the complete built-in Noto Sans KR
  // scale while Korean is active; switching languages restores that selection.
  if (!korean && SETTINGS.uiSdFontFamilyName[0] != '\0') {
    sdFontSystem.loadInterfaceFaces(SETTINGS.uiSdFontFamilyName, renderer, uiSlots, std::size(uiSlots));
  }
  if (!korean && SETTINGS.scriptSdFontFamilyName[0] != '\0') {
    sdFontSystem.loadInterfaceFaces(SETTINGS.scriptSdFontFamilyName, renderer, accentSlots, std::size(accentSlots));
  }

  const auto fillSlot = [](const int fontId, const EpdFontFamily& family) {
    if (renderer.getFontMap().count(fontId) == 0) renderer.insertFont(fontId, family);
  };
  fillSlot(MICRO_FONT_ID, korean ? ui8KoreanFontFamily : ui8FontFamily);    // 8 pt — keyboard, captions
  fillSlot(SMALL_FONT_ID, korean ? ui10KoreanFontFamily : ui10FontFamily);  // 10 pt — legends
  fillSlot(UI_10_FONT_ID, korean ? ui12KoreanFontFamily : ui12FontFamily);  // 12 pt — labels, values
  fillSlot(UI_12_FONT_ID, korean ? ui14KoreanFontFamily : ui14FontFamily);  // 14 pt — list row titles
  fillSlot(UI_14_FONT_ID, korean ? ui16KoreanFontFamily : ui16FontFamily);  // 16 pt — book titles
  fillSlot(UI_16_FONT_ID, korean ? ui16KoreanFontFamily : ui16FontFamily);
  fillSlot(UI_18_FONT_ID, korean ? ui16KoreanFontFamily : ui16FontFamily);
  fillSlot(HEADER_FONT_ID, korean ? ui16KoreanFontFamily : uiHeaderFontFamily);
  // Caveat has no Korean design. Keep the accent slots legible and size-stable
  // with the 16 px Noto Sans KR face instead of embedding duplicate Hangul.
  fillSlot(SCRIPT_SMALL_FONT_ID, korean ? ui16KoreanFontFamily : uiScriptSmallFontFamily);
  fillSlot(SCRIPT_FONT_ID, korean ? ui16KoreanFontFamily : uiScriptFontFamily);
  LOG_DBG("MAIN", "Interface slots: script=%d/%d sd=%d/%d | rows sd=%d asc=%d",
          renderer.getFontAscenderSize(SCRIPT_SMALL_FONT_ID), renderer.getFontAscenderSize(SCRIPT_FONT_ID),
          (int)renderer.isSdCardFont(SCRIPT_SMALL_FONT_ID), (int)renderer.isSdCardFont(SCRIPT_FONT_ID),
          (int)renderer.isSdCardFont(UI_12_FONT_ID), renderer.getFontAscenderSize(UI_12_FONT_ID));
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  BootDiag::markCleanShutdown(BootDiag::Shutdown::Restart);
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  BootDiag::markCleanShutdown(BootDiag::Shutdown::Restart);
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  // X4 loses its internal clock when the battery latch removes power. Keep the
  // best known epoch on SD so the next boot still has a useful visual fallback.
  halClock.saveCurrentTime();

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  BootDiag::markCleanShutdown(fromTimeout ? BootDiag::Shutdown::IdleTimeout : BootDiag::Shutdown::PowerButton);
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

bool setupDisplayAndFonts(bool seamless = false) {
  bool displayReady = false;
  for (int attempt = 1; attempt <= 3 && !displayReady; ++attempt) {
    displayReady = display.begin(seamless);
    if (!displayReady) {
      LOG_ERR("MAIN", "Display initialization attempt %d/3 failed", attempt);
      delay(50);
    }
  }
  if (!displayReady || !display.isReady()) {
    displayInitFailed = true;
    LOG_ERR("MAIN", "Display unavailable; rendering disabled to avoid a null-framebuffer crash");
    if (Storage.ready()) {
      Storage.mkdir("/.crosspoint");
      Storage.writeFile("/.crosspoint/display_init_error.txt",
                        String("InkPointX ") + CROSSPOINT_VERSION + " failed to initialize the e-ink panel");
    }
    return false;
  }
  renderer.begin();
  renderer.setFrameOverlayHook(drawSystemFrameOverlay);
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  // Before applyInterfaceFont(): the interface can be drawn in a face from the
  // card, and it can only be bound once the registry knows the card's families.
  // The other way round the whole UI silently fell back to the built-in face
  // for the rest of the session, however the user had set it.
  sdFontSystem.begin(renderer);

  applyInterfaceFont();
  LOG_DBG("MAIN", "Fonts setup");
  return true;
}

void setup() {
#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    if (!setupDisplayAndFonts(isSilentReboot)) return;
    activityManager.goToFullScreenMessage(tr(STR_SD_CARD_ERROR), EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  // Before anything can reroute the boot (a wake that goes straight back to
  // sleep, a silent reboot): report how the previous session ended.
  BootDiag::begin();

  SETTINGS.loadFromFile();
  halClock.restoreFromStorage();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  FAVORITE_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();

  // Recovery must be resolved before any automatic "USB cold boot -> sleep"
  // routing. Otherwise the very condition in which recovery is most useful
  // can power the device down before the application checks the buttons.
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterUSBPower) {
    // InputManager debounces over 20 ms. Two samples are sufficient to latch
    // a deliberately held recovery chord; the old unconditional 500 ms probe
    // delayed every normal wake even though no chord was being pressed.
    gpio.update();
    delay(25);
    gpio.update();
    if (gpio.isPressed(HalGPIO::BTN_UP) && gpio.isPressed(HalGPIO::BTN_POWER)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      if (recoveryFirmwareMode) break;
      LOG_DBG("MAIN", "Verifying power button press duration");
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP)) {
        powerManager.startDeepSleep(gpio);
      }
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      if (recoveryFirmwareMode) break;
#ifdef ENABLE_SERIAL_LOG
      // Diagnostic builds must remain awake after USB reset so the serial
      // console can exercise network routes on real hardware.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power (diagnostic build stays awake)");
      break;
#else
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting InkPoint X version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Seamless paths skip the
  // splash but still run one controller-safe fast-full update: begin() resets
  // controller RAM while the physical e-ink panel retains its old frame.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;

  if (!setupDisplayAndFonts(resume != BootResume::Splash)) {
    // A pending OTA must reset so the bootloader can roll back. On an ordinary
    // image stay alive for USB diagnostics instead of entering a blind reboot
    // loop that needlessly drains the battery.
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t imageState;
    if (esp_ota_get_state_partition(running, &imageState) == ESP_OK && imageState == ESP_OTA_IMG_PENDING_VERIFY) {
      delay(250);
      ESP.restart();
    }
    return;
  }

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears controller RAM; restore the saved frame as the X3
          // differential baseline before replacing the moon icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        makeUniqueNoThrow<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  // These stores are not required to choose or paint the first Home/Reader
  // frame. Loading them after activity routing lets the render task start
  // immediately while preserving the invariant that every store is loaded
  // before the first interactive loop can save it.
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  WIFI_STORE.loadFromFile();

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 20ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(25);
    gpio.update();
  }

  // Watchdog: the config arms the TWDT but nothing ever subscribed, so the 25
  // esp_task_wdt_reset() calls scattered through the network code were no-ops
  // and an application-level hang required the user to force-power the device.
  // The render task is deliberately not subscribed - it parks forever while
  // idle. ActivityManager never blocks loopTask behind a busy render, so a
  // one-minute window now catches real main-loop/network/SD hangs without
  // misclassifying a long chapter layout as a crash.
  {
    esp_task_wdt_config_t wdtConfig = {};
    wdtConfig.timeout_ms = 60000;
    wdtConfig.idle_core_mask = 0;
    wdtConfig.trigger_panic = true;
    esp_task_wdt_reconfigure(&wdtConfig);
    enableLoopWDT();
  }

  // Boot/recovery probing intentionally samples held keys. None of those edges
  // belongs to the first interactive screen.
  bootPowerHeld = gpio.isPressed(HalGPIO::BTN_POWER);
  gpio.clearInputEvents();
  allowSleepAt = millis() + 2000;
  bootCoreInitialized = true;
}

// Arduino's default marks a pending OTA image valid before setup() even runs,
// which makes bootloader rollback useless: a firmware that boots and panics
// immediately can never roll back. Returning true here defers the decision to
// markOtaValidOnceHealthy() below.
extern "C" bool verifyRollbackLater() { return true; }

namespace {
// OTA health is a set of functional milestones, not merely elapsed uptime.
// Until all of them pass, any reset returns the device to the previous slot.
void markOtaValidOnceHealthy() {
  static bool done = false;
  static uint32_t healthyLoopCount = 0;
  if (done) return;
  if (bootCoreInitialized && Storage.ready() && display.isReady() && activityManager.hasCompletedFrame()) {
    ++healthyLoopCount;
  }
  if (millis() < 30000 || healthyLoopCount < 100) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  const esp_err_t stateResult = esp_ota_get_state_partition(running, &state);
  if (stateResult != ESP_OK) {
    LOG_ERR("OTA", "Could not read running image state: %s", esp_err_to_name(stateResult));
    return;
  }
  if (state == ESP_OTA_IMG_PENDING_VERIFY) {
    const esp_err_t markResult = esp_ota_mark_app_valid_cancel_rollback();
    if (markResult != ESP_OK) {
      LOG_ERR("OTA", "Could not mark healthy image valid: %s", esp_err_to_name(markResult));
      return;
    }
    LOG_INF("OTA", "Image marked valid after storage, display and main-loop health milestones");
  }
  done = true;
}
}  // namespace

void loop() {
  if (displayInitFailed) {
    delay(1000);
    return;
  }
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
#ifdef ENABLE_SERIAL_LOG
  static unsigned long lastMemPrint = 0;
#endif

  gpio.update();
  if (bootPowerHeld) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) {
      gpio.clearInputEvents();
      delay(5);
      return;
    }
    // Suppress the wake-release edge and start the long-press sleep window
    // from the first genuinely interactive sample.
    bootPowerHeld = false;
    gpio.clearInputEvents();
    allowSleepAt = millis() + 2000;
  }
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());
  BootDiag::tick();
  markOtaValidOnceHealthy();

  renderer.setFadingFix(SETTINGS.fadingFix);

#ifdef ENABLE_SERIAL_LOG
  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        // Snapshot the framebuffer under the same lock used by the render task.
        // Without this, an activity redraw can replace the buffer while the
        // relatively slow USB transfer is in progress, producing a torn image
        // containing parts of two different screens.
        RenderLock renderLock;
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.setTxTimeoutMs(1000);
        logSerial.printf("SCREENSHOT_START:%u\n", (unsigned)bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        uint32_t bytesSent = 0;
        const unsigned long transferStartedAt = millis();
        unsigned long lastProgressAt = transferStartedAt;
        constexpr unsigned long SCREENSHOT_STALL_TIMEOUT_MS = 3000;
        constexpr unsigned long SCREENSHOT_TOTAL_TIMEOUT_MS = 15000;
        while (bytesSent < bufferSize) {
          const size_t chunkSize = std::min<uint32_t>(64, bufferSize - bytesSent);
          const size_t written = logSerial.write(buf + bytesSent, chunkSize);
          if (written == 0) {
            if (millis() - lastProgressAt >= SCREENSHOT_STALL_TIMEOUT_MS ||
                millis() - transferStartedAt >= SCREENSHOT_TOTAL_TIMEOUT_MS) {
              LOG_ERR("MAIN", "Screenshot transfer timed out after %u/%u bytes", static_cast<unsigned>(bytesSent),
                      static_cast<unsigned>(bufferSize));
              break;
            }
            delay(1);
            continue;
          }
          bytesSent += written;
          lastProgressAt = millis();
          delay(1);
        }
        logSerial.flush();
        if (bytesSent == bufferSize) {
          logSerial.printf("SCREENSHOT_END\n");
        } else {
          logSerial.printf("SCREENSHOT_ABORT:%u\n", static_cast<unsigned>(bytesSent));
        }
        logSerial.flush();
        logSerial.setTxTimeoutMs(1);
#if LOG_LEVEL >= 2
      } else if (cmd.startsWith("PROFILE_UIFONT")) {
        // CMD:PROFILE_UIFONT[:Family] — bind the interface to a card family
        // (no argument returns to the built-in face). Buttons cannot be
        // pressed over serial, and this is the only way to exercise the whole
        // path: registry lookup, face load, slot binding, repaint.
        const int sep = cmd.indexOf(':');
        const String family = sep < 0 ? String() : cmd.substring(sep + 1);
        strncpy(SETTINGS.uiSdFontFamilyName, family.c_str(), CrossPointSettings::SD_FONT_NAME_MAX - 1);
        SETTINGS.uiSdFontFamilyName[CrossPointSettings::SD_FONT_NAME_MAX - 1] = '\0';
        applyInterfaceFont();
        SETTINGS.saveToFile();  // as the picker does, so a sleep-wake keeps it
        LOG_INF("MAIN", "Profile route: interface font = '%s'", SETTINGS.uiSdFontFamilyName);
        activityManager.goHome();
      } else if (cmd.startsWith("PROFILE_ACCENTFONT")) {
        const int sep = cmd.indexOf(':');
        const String family = sep < 0 ? String() : cmd.substring(sep + 1);
        strncpy(SETTINGS.scriptSdFontFamilyName, family.c_str(), CrossPointSettings::SD_FONT_NAME_MAX - 1);
        SETTINGS.scriptSdFontFamilyName[CrossPointSettings::SD_FONT_NAME_MAX - 1] = '\0';
        applyInterfaceFont();
        SETTINGS.saveToFile();  // as the picker does, so a sleep-wake keeps it
        LOG_INF("MAIN", "Profile route: accent font = '%s'", SETTINGS.scriptSdFontFamilyName);
        activityManager.goHome();
      } else if (cmd == "PROFILE_SLEEP") {
        // Drives the power button's sleep path end to end without a finger on
        // the button: the teardown, the sleep frame, the panel shutdown and
        // esp_deep_sleep_start(). A panic anywhere in there would look exactly
        // like "the power button rebooted the device instead of sleeping it".
        LOG_INF("MAIN", "Profile route: deep sleep (power-button path)");
        enterDeepSleep(false);
      } else if (cmd == "PROFILE_OTA") {
        // Verification route for the over-the-air update path: reaching it
        // through the UI needs several button presses this console cannot make.
        activityManager.replaceActivity(makeUniqueNoThrow<OtaUpdateActivity>(renderer, mappedInputManager));
        LOG_DBG("MAIN", "Profile route: OTA update");
      } else if (cmd == "PROFILE_FONTS") {
        activityManager.replaceActivity(makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager));
        LOG_DBG("MAIN", "Profile route: font catalog");
      } else if (cmd == "PROFILE_FONT_FETCH") {
        constexpr const char* testUrl =
            "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m1-b4/"
            "Alegreya_12.cpfont";
        constexpr const char* testPath = "/.font_transport_test.cpfont";
        const auto result = HttpDownloader::downloadToFile(testUrl, testPath, nullptr);
        size_t downloadedSize = 0;
        HalFile testFile = Storage.open(testPath);
        if (testFile) {
          downloadedSize = testFile.size();
          testFile.close();
        }
        LOG_DBG("MAIN", "Profile font asset fetch: result=%d size=%u", static_cast<int>(result),
                static_cast<unsigned>(downloadedSize));
        Storage.remove(testPath);
      } else if (cmd == "PROFILE_REINDEX") {
        // Wipes the most recent book's cache and reopens it — forces the full
        // chapter re-index path, which is where the light-sleep RenderLock
        // regression wedged the main loop.
        if (!RECENT_BOOKS.getBooks().empty()) {
          const auto path = RECENT_BOOKS.getBooks().front().path;
          // Force the position to a mid-book chapter after the wipe: reopening
          // at the one-page cover chapter indexes in seconds and never
          // exercises the long re-index path this route exists for.
          const std::string cacheDir = getBookCachePath(path);
          clearBookCache(path);
          Storage.ensureDirectoryExists(cacheDir.c_str());
          const uint8_t forced[6] = {24, 0, 1, 0, 0, 0};  // spine=24, page=1
          ProgressFile::writeAtomic(cacheDir, forced, sizeof(forced));
          activityManager.goToReader(path);
          LOG_DBG("MAIN", "Profile route: reindex %s", path.c_str());
        }
      } else if (cmd == "PROFILE_READER") {
        // Opens the most recent book — the only way to reach a reading page
        // from the console for framebuffer verification.
        if (!RECENT_BOOKS.getBooks().empty()) {
          activityManager.goToReader(RECENT_BOOKS.getBooks().front().path);
          LOG_DBG("MAIN", "Profile route: Reader");
        } else {
          LOG_DBG("MAIN", "Profile route: Reader - no recent books");
        }
      } else if (cmd == "PROFILE_REDRAW") {
        // Development-only latency probe. It exercises the exact active
        // activity render path without changing UI state.
        activityManager.requestUpdate();
        LOG_DBG("MAIN", "Profile redraw requested");
      } else if (cmd.startsWith("PROFILE_NAV_DOWN")) {
        // Inject a burst without waiting for panel BUSY so the input backlog
        // and frame coalescing path can be verified deterministically over USB.
        const int sep = cmd.indexOf(':');
        const int count = std::clamp(sep < 0 ? 1 : cmd.substring(sep + 1).toInt(), 1L, 24L);
        for (int i = 0; i < count; ++i) gpio.enqueueSyntheticClick(HalGPIO::BTN_DOWN);
        LOG_DBG("MAIN", "Profile input: queued %d Down clicks", count);
      } else if (cmd == "PROFILE_CONFIRM") {
        gpio.enqueueSyntheticClick(SETTINGS.frontButtonConfirm);
        LOG_DBG("MAIN", "Profile input: queued Confirm click");
      } else if (cmd == "PROFILE_LIBRARY") {
        activityManager.goHome(HomeMenuItem::LIBRARY);
        LOG_DBG("MAIN", "Profile route: Library");
      } else if (cmd == "PROFILE_BOOKS") {
        activityManager.goToLibrary();
        LOG_DBG("MAIN", "Profile route: Books");
      } else if (cmd == "PROFILE_FILES") {
        activityManager.goToFileBrowser();
        LOG_DBG("MAIN", "Profile route: Files");
      } else if (cmd == "PROFILE_GALLERY") {
        activityManager.goToGallery();
        LOG_DBG("MAIN", "Profile route: Gallery");
      } else if (cmd == "PROFILE_HOME_SETTINGS") {
        activityManager.goHome(HomeMenuItem::SETTINGS_MENU);
        LOG_DBG("MAIN", "Profile route: Settings hub");
      } else if (cmd == "PROFILE_HOME") {
        activityManager.goHome();
        LOG_DBG("MAIN", "Profile route: Home");
      } else if (cmd == "PROFILE_SETTINGS") {
        activityManager.goToSettings();
        LOG_DBG("MAIN", "Profile route: Settings");
#endif
      }
    }
  }
#endif

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Removed in 2.0.2: a critical-battery guard that force-slept the device
  // below 2%. Its premise was sound (a brownout mid-write is how settings
  // files used to vanish) but its input is not: on the X4 the reading is an
  // ADC divider smoothed in software, and it sags under a panel refresh, at
  // 10 MHz in power-saving mode, and after a sleep wake. Acting on it powers
  // a working device off, which on e-ink is indistinguishable from a freeze —
  // the panel keeps the last frame and the next press looks like a reboot.
  // A guard that turns the device off must be at least as trustworthy as the
  // failure it prevents. This one wasn't, and the failure is rarer.

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    // The X4's single-pass D7 clean removes accumulated differential residue
    // without the conspicuous multi-phase black flash of FULL (F7).
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  [[maybe_unused]] const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Removed in 2.0.2: timer-wakeup esp_light_sleep_start() in place of this
      // delay. It saved real idle current, and it cost the firmware its
      // reliability on battery — the only configuration it ran in, and the one
      // configuration a USB-tethered bench cannot observe. Halting the clocks
      // this deep touches the ADC ladder every button rides on, the panel's
      // SPI state and the power rails, and each round of hardening produced a
      // new field failure mode instead of a quiet device. A plain delay is
      // 50 ms of WFI at 10 MHz: unglamorous, and correct on hardware I cannot
      // instrument. It can come back the day it can be measured on battery
      // with a current probe rather than reasoned about.
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
