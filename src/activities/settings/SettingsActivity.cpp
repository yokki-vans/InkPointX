#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "DeviceInfoActivity.h"
#include "DictionaryPickerActivity.h"
#include "FontDownloadActivity.h"
#include "FontSelectionActivity.h"
#include "InterfaceFont.h"
#include "InterfaceFontSelectActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OpdsServerStore.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SettingsReset.h"
#include "StatusBarSettingsActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/LibraryActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/SleepImageInstaller.h"

const StrId SettingsActivity::categoryNames[CATEGORY_COUNT] = {
    StrId::STR_SETTINGS_INTERFACE, StrId::STR_SETTINGS_POWER,   StrId::STR_SETTINGS_READING,
    StrId::STR_SETTINGS_CONTROLS,  StrId::STR_SETTINGS_LIBRARY, StrId::STR_SETTINGS_NETWORK,
    StrId::STR_SETTINGS_SYSTEM,
};

void SettingsActivity::rebuildSettingsLists() {
  for (auto& settings : categorySettings) settings.clear();

  auto& interfaceSettings = categorySettings[0];
  auto& powerSettings = categorySettings[1];
  auto& readingSettings = categorySettings[2];
  auto& controlsSettings = categorySettings[3];
  auto& librarySettings = categorySettings[4];
  auto& networkSettings = categorySettings[5];
  auto& systemSettings = categorySettings[6];

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();

  for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      const bool isPowerSetting =
          setting.nameId == StrId::STR_SLEEP_SCREEN || setting.nameId == StrId::STR_SLEEP_COVER_MODE ||
          setting.nameId == StrId::STR_SLEEP_COVER_FILTER || setting.nameId == StrId::STR_QUICK_RESUME_TIMEOUT;
      (isPowerSetting ? powerSettings : interfaceSettings).push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readingSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
        powerSettings.push_back(setting);
      } else if (setting.nameId == StrId::STR_SHOW_HIDDEN_FILES ||
                 setting.nameId == StrId::STR_REMOVE_READ_FROM_RECENTS ||
                 setting.nameId == StrId::STR_MOVE_FINISHED_TO_READ) {
        librarySettings.push_back(setting);
      }
      // Interface font uses its dedicated preview selector below.
    } else if (setting.category == StrId::STR_SETTINGS_NETWORK) {
      networkSettings.push_back(setting);
    }
  }

  // Interface. Inserted before the interface-font row so it lands after it:
  // the accent face is chosen the same way, from the same list.
  interfaceSettings.insert(interfaceSettings.begin(),
                           SettingInfo::Action(StrId::STR_ACCENT_FONT, SettingAction::AccentFont));
  interfaceSettings.insert(interfaceSettings.begin(),
                           SettingInfo::Action(StrId::STR_INTERFACE_FONT, SettingAction::InterfaceFont));
  interfaceSettings.insert(interfaceSettings.begin(),
                           SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));

  // Screen and power
  const auto sleepScreen = std::find_if(powerSettings.begin(), powerSettings.end(), [](const SettingInfo& setting) {
    return setting.nameId == StrId::STR_SLEEP_SCREEN;
  });
  powerSettings.insert(sleepScreen == powerSettings.end() ? powerSettings.begin() : sleepScreen + 1,
                       SettingInfo::Action(StrId::STR_LOCK_SCREEN_IMAGE, SettingAction::SelectSleepImage));

  // Reading
  readingSettings.insert(readingSettings.begin() + std::min<size_t>(1, readingSettings.size()),
                         SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  readingSettings.insert(readingSettings.begin(),
                         SettingInfo::Action(StrId::STR_CHOOSE_DICTIONARY, SettingAction::Dictionary));
  readingSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Library and files
  librarySettings.push_back(SettingInfo::Action(StrId::STR_RESCAN_LIBRARY, SettingAction::RescanLibrary));

  // Controls
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));

  // Network, sync and catalogues
  networkSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  networkSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  networkSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  if (OPDS_STORE.hasServers()) {
    networkSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::BrowseOPDS));
  }

  // Maintenance and device information
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_DEVICE_INFO, SettingAction::DeviceInfo));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_RESET_SETTINGS, SettingAction::ResetSettings));

  currentSettings = &categorySettings[selectedCategoryIndex];
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  selectedCategoryIndex = std::clamp(initialCategoryIndex, 0, CATEGORY_COUNT - 1);
  selectedSettingIndex = 1;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() { loopSubmenu(); }

void SettingsActivity::loopSubmenu() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    if (returnToCaller) {
      finish();
    } else {
      onGoHome(homeMenuItemForCategory());
    }
    return;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, 42);
  buttonNavigator.onNextPress([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex - 1, settingsCount) + 1;
    requestUpdate();
  });
  buttonNavigator.onPreviousPress([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex - 1, settingsCount) + 1;
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedSettingIndex = ButtonNavigator::nextPageIndex(selectedSettingIndex - 1, settingsCount, pageItems) + 1;
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedSettingIndex = ButtonNavigator::previousPageIndex(selectedSettingIndex - 1, settingsCount, pageItems) + 1;
    requestUpdate();
  });
}

HomeMenuItem SettingsActivity::homeMenuItemForCategory() const {
  constexpr std::array<HomeMenuItem, CATEGORY_COUNT> menuItems = {
      HomeMenuItem::SETTINGS_MENU,     HomeMenuItem::SETTINGS_POWER,   HomeMenuItem::SETTINGS_READING,
      HomeMenuItem::SETTINGS_CONTROLS, HomeMenuItem::SETTINGS_LIBRARY, HomeMenuItem::SETTINGS_NETWORK,
      HomeMenuItem::SETTINGS_SYSTEM,
  };
  return menuItems[std::clamp(selectedCategoryIndex, 0, CATEGORY_COUNT - 1)];
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
  const bool sleepScreenChanged = setting.nameId == StrId::STR_SLEEP_SCREEN;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  // Route the reader font to its picker before any ENUM branch. The registry-aware
  // entry is only substituted when SD fonts exist, so on a stock device this fell
  // through to the cycle-in-place branch below — while render() still drew a
  // chevron and labelled Confirm "Select", promising a submenu that never opened.
  if (setting.nameId == StrId::STR_FONT_FAMILY) {
    startActivityForResult(makeUniqueNoThrow<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                           [this](const ActivityResult&) {
                             SETTINGS.saveToFile();
                             rebuildSettingsLists();
                           });
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    setting.valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // int, not int8_t: the field is uint8_t and a range above 127 would wrap.
    const int currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(makeUniqueNoThrow<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(makeUniqueNoThrow<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(makeUniqueNoThrow<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(makeUniqueNoThrow<OpdsServerListActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 OPDS_STORE.loadFromFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Network:
        startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(makeUniqueNoThrow<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(makeUniqueNoThrow<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(makeUniqueNoThrow<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(makeUniqueNoThrow<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::InterfaceFont:
        startActivityForResult(makeUniqueNoThrow<InterfaceFontSelectActivity>(
                                   renderer, mappedInput, InterfaceFontSelectActivity::Target::Interface),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::AccentFont:
        startActivityForResult(makeUniqueNoThrow<InterfaceFontSelectActivity>(
                                   renderer, mappedInput, InterfaceFontSelectActivity::Target::Accent),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::SelectSleepImage:
        startActivityForResult(makeUniqueNoThrow<FileBrowserActivity>(renderer, mappedInput, "/",
                                                                      FileBrowserActivity::Mode::PickSleepImage),
                               [this](const ActivityResult& result) {
                                 if (result.isCancelled || !std::holds_alternative<FilePathResult>(result.data)) return;

                                 GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
                                 renderer.displayBuffer();

                                 const auto& path = std::get<FilePathResult>(result.data).path;
                                 const bool crop =
                                     SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
                                 const bool installed = SleepImageInstaller::install(path, crop);
                                 if (installed) {
                                   SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
                                   SETTINGS.saveToFile();
                                   GUI.drawPopup(renderer, tr(STR_DONE));
                                 } else {
                                   GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
                                 }
                                 renderer.displayBuffer();
                                 delay(900);
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::BrowseOPDS:
        activityManager.goToBrowser();
        break;
      case SettingAction::DeviceInfo:
        startActivityForResult(makeUniqueNoThrow<DeviceInfoActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ResetSettings:
        startActivityForResult(makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_RESET_SETTINGS),
                                                                       tr(STR_RESET_SETTINGS_WARNING)),
                               [](const ActivityResult& result) {
                                 if (result.isCancelled) return;
                                 resetFirmwareConfiguration();
                               });
        break;
      case SettingAction::Dictionary:
        startActivityForResult(makeUniqueNoThrow<DictionaryPickerActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::RescanLibrary:
        if (LibraryActivity::invalidateIndex())
          GUI.drawPopup(renderer, tr(STR_DONE));
        else
          GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
        renderer.displayBuffer();
        delay(700);
        requestUpdate();
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  if (setting.valuePtr == &CrossPointSettings::uiFontFamily) {
    applyInterfaceFont();
  }
  if (setting.valuePtr == &CrossPointSettings::showButtonHints) {
    UITheme::getInstance().reload();
  }
  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResult(
      makeUniqueNoThrow<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, StrId::STR_SLEEP_TIMER_STEP_HINT,
          SETTINGS.sleepTimeoutMinutes, CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES,
          CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5, StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true,
          StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 I18N.get(categoryNames[selectedCategoryIndex]));

  const auto& settings = *currentSettings;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, std::max(0, UITheme::getListContentBottom(renderer, true) - listTop)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::ACTION && setting.nameId == StrId::STR_LOCK_SCREEN_IMAGE &&
                   Storage.exists(SleepImageInstaller::INSTALLED_IMAGE_PATH)) {
          valueText = tr(STR_SELECTED);
        }
        return valueText;
      },
      true, nullptr,
      [&settings](int index) {
        const auto& setting = settings[index];
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          return SETTINGS.*(setting.valuePtr) ? UIAccessory::ToggleOn : UIAccessory::ToggleOff;
        }
        const bool opensSubmenu = setting.nameId == StrId::STR_TIME_TO_SLEEP ||
                                  setting.nameId == StrId::STR_FONT_FAMILY || setting.type == SettingType::ACTION;
        return opensSubmenu ? UIAccessory::Chevron : UIAccessory::None;
      });

  GUI.drawFooterCounter(renderer, selectedSettingIndex - 1, settingsCount);

  // Guard the index: an empty category, or a selection that has not yet been
  // clamped after the lists were rebuilt, would otherwise read out of range.
  const int selectedRow = selectedSettingIndex - 1;
  bool opensSubmenu = false;
  if (selectedRow >= 0 && selectedRow < static_cast<int>(currentSettings->size())) {
    const auto& selectedSetting = (*currentSettings)[selectedRow];
    opensSubmenu = selectedSetting.nameId == StrId::STR_TIME_TO_SLEEP ||
                   selectedSetting.nameId == StrId::STR_FONT_FAMILY || selectedSetting.type == SettingType::ACTION;
  }
  const auto confirmLabel = opensSubmenu ? tr(STR_SELECT) : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
