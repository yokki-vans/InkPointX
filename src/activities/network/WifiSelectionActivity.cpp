#include "WifiSelectionActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "achievements/AchievementSystem.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void WifiSelectionActivity::onEnter() {
  Activity::onEnter();

  // Load saved WiFi credentials - SD card operations need lock as we use SPI
  // for both
  {
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }

  // Reset state
  selectedNetworkIndex = 0;
  networks.clear();
  state = WifiSelectionState::SCANNING;
  selectedSSID.clear();
  connectedIP.clear();
  connectionError.clear();
  enteredPassword.clear();
  usedSavedPassword = false;
  savePromptSelection = 0;
  forgetPromptSelection = 0;
  autoConnecting = false;

  // Cache MAC address for display
  uint8_t mac[6];
  WiFi.macAddress(mac);
  // Raw hex only: with the localized "MAC address:" prefix the string was
  // wider than the panel and the last octet was cut off.
  char macStr[64];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  cachedMacAddress = std::string(macStr);

  // Trigger first update to show scanning message
  requestUpdate();

  // Attempt to auto-connect to the last network
  if (allowAutoConnect) {
    const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
    if (!lastSsid.empty()) {
      const auto* cred = WIFI_STORE.findCredential(lastSsid);
      if (cred) {
        LOG_DBG("WIFI", "Attempting to auto-connect to %s", lastSsid.c_str());
        selectedSSID = cred->ssid;
        enteredPassword = cred->password;
        selectedRequiresPassword = !cred->password.empty();
        usedSavedPassword = true;
        autoConnecting = true;
        attemptConnection();
        requestUpdate();
        return;
      }
    }
  }

  // Fallback to scanning
  startWifiScan();
}

void WifiSelectionActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WIFI", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  // Stop any ongoing WiFi scan
  LOG_DBG("WIFI", "Deleting WiFi scan...");
  WiFi.scanDelete();
  LOG_DBG("WIFI", "Free heap after scanDelete: %d bytes", ESP.getFreeHeap());

  // Note: We do NOT disconnect WiFi here - the parent activity
  // (CrossPointWebServerActivity) manages WiFi connection state. We just clean
  // up the scan and task.

  LOG_DBG("WIFI", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void WifiSelectionActivity::startWifiScan() {
  autoConnecting = false;
  state = WifiSelectionState::SCANNING;
  scanStartTime = millis();
  networks.clear();
  requestUpdate();

  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Start async scan
  WiFi.scanNetworks(true);  // true = async scan
}

void WifiSelectionActivity::processWifiScanResults() {
  const int16_t scanResult = WiFi.scanComplete();

  if (scanResult == WIFI_SCAN_RUNNING) {
    // A wedged scan used to hang this screen forever — the driver has no
    // deadline of its own, so give it one and fall through to the empty list.
    if (millis() - scanStartTime > SCAN_TIMEOUT_MS) {
      WiFi.scanDelete();
      networks.clear();
      state = WifiSelectionState::NETWORK_LIST;
      requestUpdate();
    }
    return;
  }

  if (scanResult == WIFI_SCAN_FAILED) {
    state = WifiSelectionState::NETWORK_LIST;
    requestUpdate();
    return;
  }

  // Scan complete, process results — deduplicate in-place, keeping strongest signal
  networks.clear();
  networks.reserve(scanResult);

  for (int i = 0; i < scanResult; i++) {
    char ssid[33];
    strlcpy(ssid, WiFi.SSID(i).c_str(), sizeof(ssid));
    const int32_t rssi = WiFi.RSSI(i);

    // Skip hidden networks (empty SSID)
    if (ssid[0] == '\0') {
      continue;
    }

    auto it =
        std::find_if(networks.begin(), networks.end(), [&ssid](const WifiNetworkInfo& n) { return n.ssid == ssid; });
    if (it == networks.end()) {
      WifiNetworkInfo network;
      network.ssid = ssid;
      network.rssi = rssi;
      network.isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      network.hasSavedPassword = WIFI_STORE.hasSavedCredential(network.ssid);
      networks.push_back(std::move(network));
    } else if (rssi > it->rssi) {
      it->rssi = rssi;
      it->isEncrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
  }

  // Sort: saved-password networks first, then by signal strength (strongest first)
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& a, const WifiNetworkInfo& b) {
    if (a.hasSavedPassword != b.hasSavedPassword) {
      return a.hasSavedPassword;
    }
    return a.rssi > b.rssi;
  });

  WiFi.scanDelete();
  state = WifiSelectionState::NETWORK_LIST;
  selectedNetworkIndex = 0;
  requestUpdate();
}

void WifiSelectionActivity::selectNetwork(const int index) {
  if (index < 0 || index >= static_cast<int>(networks.size())) {
    return;
  }

  const auto& network = networks[index];
  selectedSSID = network.ssid;
  selectedRequiresPassword = network.isEncrypted;
  usedSavedPassword = false;
  enteredPassword.clear();
  autoConnecting = false;

  // Check if we have saved credentials for this network
  const auto* savedCred = WIFI_STORE.findCredential(selectedSSID);
  if (savedCred && !savedCred->password.empty()) {
    // Use saved password - connect directly
    enteredPassword = savedCred->password;
    usedSavedPassword = true;
    LOG_DBG("WiFi", "Using saved password for %s, length: %zu", selectedSSID.c_str(), enteredPassword.size());
    attemptConnection();
    return;
  }

  if (selectedRequiresPassword) {
    // Show password entry
    state = WifiSelectionState::PASSWORD_ENTRY;
    // Don't allow screen updates while changing activity
    startActivityForResult(makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_WIFI_PASSWORD),
                                                                    "",  // No initial text
                                                                    64,  // Max password length
                                                                    InputType::Password),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               state = WifiSelectionState::NETWORK_LIST;
                               return;
                             }
                             enteredPassword = std::get<KeyboardResult>(result.data).text;
                             // An empty key on a secured network fell through to
                             // WiFi.begin(ssid) with no key at all. The driver
                             // rarely reports WL_CONNECT_FAILED for that, so the
                             // user waited out the whole connection timeout to be
                             // told only that it failed. No length rule beyond
                             // this: WEP keys are legitimately 5 or 13 characters.
                             if (enteredPassword.empty()) {
                               connectionError = tr(STR_ENTER_WIFI_PASSWORD);
                               state = WifiSelectionState::NETWORK_LIST;
                               requestUpdate();
                               return;
                             }
                             // state will be updated in next loop iteration
                           });
  } else {
    // Connect directly for open networks
    attemptConnection();
  }
}

void WifiSelectionActivity::attemptConnection() {
  state = autoConnecting ? WifiSelectionState::AUTO_CONNECTING : WifiSelectionState::CONNECTING;
  connectedIP.clear();
  connectionError.clear();
  requestUpdate();

  WiFi.persistent(false);  // Credentials are managed by WifiCredentialStore; suppress SDK NVS auto-connect
  WiFi.mode(WIFI_STA);
  // Keep the station radio enabled while aborting an SDK auto-connect.  Passing
  // wifioff=true here forced an avoidable OFF -> STA transition immediately
  // before WiFi.begin(), which is particularly unreliable on the X3.
  WiFi.disconnect(false, true);
  delay(100);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);

  // Arduino's default fast scan stops at the first AP whose SSID matches.  On
  // mesh and combined 2.4/5 GHz networks that can be a weak or unusable BSSID.
  // Scan every channel and let the ESP32-C3 associate with the strongest match.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  // Set hostname so routers show "InkPointX-AABBCCDDEEFF" instead of "esp32-XXXXXXXXXXXX"
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String hostname = "InkPointX-" + mac;
  WiFi.setHostname(hostname.c_str());

  connectionStartTime = millis();
  if (selectedRequiresPassword && !enteredPassword.empty()) {
    WiFi.begin(selectedSSID.c_str(), enteredPassword.c_str());
  } else {
    WiFi.begin(selectedSSID.c_str());
  }
}

void WifiSelectionActivity::checkConnectionStatus() {
  if (state != WifiSelectionState::CONNECTING && state != WifiSelectionState::AUTO_CONNECTING) {
    return;
  }

  const wl_status_t status = WiFi.status();
  const unsigned long elapsed = millis() - connectionStartTime;

  if (status == WL_CONNECTED) {
    // Association can complete before DHCP has assigned an address.  Starting
    // the web server at 0.0.0.0 made a valid connection look broken, so wait
    // for the lease within the same bounded connection deadline.
    IPAddress ip = WiFi.localIP();
    if (ip == IPAddress(0, 0, 0, 0)) {
      if (elapsed <= CONNECTION_TIMEOUT_MS) return;
    } else {
      char ipStr[16];
      snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
      connectedIP = ipStr;
      autoConnecting = false;

      // X3's DS3231 only needs its initial sync. X4 has no always-powered RTC, so
      // its software clock must be refreshed after every cold boot.
      if ((halClock.isAvailable() && !SETTINGS.clockHasBeenSynced) || halClock.needsNetworkSync()) {
        if (halClock.syncFromNTP()) {
          SETTINGS.clockHasBeenSynced = 1;
          SETTINGS.saveToFile();
        }
      }

      // Save this as the last connected network - SD card operations need lock as
      // we use SPI for both
      {
        RenderLock lock(*this);
        WIFI_STORE.setLastConnectedSsid(selectedSSID);
      }

      // If we entered a new password, ask if user wants to save it
      // Otherwise, immediately complete so parent can start web server
      if (!usedSavedPassword && !enteredPassword.empty()) {
        state = WifiSelectionState::SAVE_PROMPT;
        savePromptSelection = 0;  // Default to "Yes"
        requestUpdate();
      } else {
        // Using saved password or open network - complete immediately
        LOG_DBG("WIFI",
                "Connected with saved/open credentials, "
                "completing immediately");
        onComplete(true);
      }
      return;
    }
  }

  if (elapsed >= CONNECTION_FAILURE_GRACE_MS && (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL)) {
    connectionError = tr(STR_ERROR_GENERAL_FAILURE);
    if (status == WL_NO_SSID_AVAIL) {
      connectionError = tr(STR_ERROR_NETWORK_NOT_FOUND);
    }
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }

  // Check for timeout
  if (elapsed > CONNECTION_TIMEOUT_MS) {
    WiFi.disconnect(false, false);
    connectionError = tr(STR_ERROR_CONNECTION_TIMEOUT);
    state = WifiSelectionState::CONNECTION_FAILED;
    requestUpdate();
    return;
  }
}

void WifiSelectionActivity::loop() {
  // Check scan progress. Back abandons the scan instead of being ignored —
  // this used to be a legend-less state with no exit.
  if (state == WifiSelectionState::SCANNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      WiFi.scanDelete();
      onComplete(false);
      return;
    }
    processWifiScanResults();
    return;
  }

  // Check connection progress; Back aborts the attempt.
  if (state == WifiSelectionState::CONNECTING || state == WifiSelectionState::AUTO_CONNECTING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      WiFi.disconnect();
      autoConnecting = false;
      if (networks.empty()) {
        startWifiScan();
      } else {
        state = WifiSelectionState::NETWORK_LIST;
        requestUpdate();
      }
      return;
    }
    checkConnectionStatus();
    return;
  }

  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    // Reach here once password entry finished in subactivity
    attemptConnection();
    return;
  }

  // Handle save prompt state
  if (state == WifiSelectionState::SAVE_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (savePromptSelection > 0) {
        savePromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (savePromptSelection < 1) {
        savePromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (savePromptSelection == 0) {
        // User chose "Yes" - save the password
        RenderLock lock(*this);
        WIFI_STORE.addCredential(selectedSSID, enteredPassword);
      }
      // Complete - parent will start web server
      onComplete(true);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip saving, complete anyway
      onComplete(true);
    }
    return;
  }

  // Handle forget prompt state (connection failed with saved credentials)
  if (state == WifiSelectionState::FORGET_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        RenderLock lock(*this);
        // User chose "Forget network" - forget the network
        WIFI_STORE.removeCredential(selectedSSID);
        // Update the network list to reflect the change
        const auto network = find_if(networks.begin(), networks.end(),
                                     [this](const WifiNetworkInfo& net) { return net.ssid == selectedSSID; });
        if (network != networks.end()) {
          network->hasSavedPassword = false;
        }
      }
      // Go back to network list (whether Cancel or Forget network was
      // selected) without discarding the scan the user already has.
      returnToNetworkList();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Skip forgetting, go back to network list
      returnToNetworkList();
    }
    return;
  }

  // Handle connected state (should not normally be reached - connection
  // completes immediately)
  if (state == WifiSelectionState::CONNECTED) {
    // Safety fallback - immediately complete
    onComplete(true);
    return;
  }

  // Handle connection failed state
  if (state == WifiSelectionState::CONNECTION_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Retry the same network — the legend used to say "Done" here while the
      // button behaved exactly like Back.
      attemptConnection();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // If we were auto-connecting or using a saved credential, offer to forget
      // the network
      if (autoConnecting || usedSavedPassword) {
        autoConnecting = false;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
      } else {
        // Go back to network list on failure for non-saved credentials
        state = WifiSelectionState::NETWORK_LIST;
      }
      requestUpdate();
      return;
    }
  }

  // Handle network list state
  if (state == WifiSelectionState::NETWORK_LIST) {
    // Check for Back button to exit (cancel)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onComplete(false);
      return;
    }

    // Check for Confirm button to select network or rescan
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!networks.empty()) {
        selectNetwork(selectedNetworkIndex);
      } else {
        startWifiScan();
      }
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      startWifiScan();
      return;
    }

    const bool leftPressed = mappedInput.wasPressed(MappedInputManager::Button::Left);
    if (leftPressed) {
      const bool hasSavedPassword = !networks.empty() && networks[selectedNetworkIndex].hasSavedPassword;
      if (hasSavedPassword) {
        selectedSSID = networks[selectedNetworkIndex].ssid;
        state = WifiSelectionState::FORGET_PROMPT;
        forgetPromptSelection = 0;  // Default to "Cancel"
        requestUpdate();
        return;
      }
    }

    // Handle navigation
    buttonNavigator.onNext([this] {
      selectedNetworkIndex = ButtonNavigator::nextIndex(selectedNetworkIndex, networks.size());
      requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
      selectedNetworkIndex = ButtonNavigator::previousIndex(selectedNetworkIndex, networks.size());
      requestUpdate();
    });
  }
}

std::string WifiSelectionActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // Convert RSSI to signal bars representation
  if (rssi >= -50) {
    return "||||";  // Excellent
  }
  if (rssi >= -60) {
    return " |||";  // Good
  }
  if (rssi >= -70) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}

void WifiSelectionActivity::render(RenderLock&&) {
  // Don't render if we're in PASSWORD_ENTRY state - we're just transitioning
  // from the keyboard subactivity back to the main activity
  if (state == WifiSelectionState::PASSWORD_ENTRY) {
    return;
  }

  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  // Draw header. The network count used to share the header with the title
  // and both halves truncated; the count now sits in the subheader next to
  // the MAC, and the title keeps the full lane.
  char countStr[32];
  snprintf(countStr, sizeof(countStr), tr(STR_NETWORKS_FOUND), networks.size());
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_WIFI_NETWORKS));
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.subHeaderHeight},
      state == WifiSelectionState::NETWORK_LIST ? countStr : "", cachedMacAddress.c_str());

  switch (state) {
    case WifiSelectionState::AUTO_CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::SCANNING:
      renderConnecting(&screen, &metrics);  // Reuse connecting screen with different message
      break;
    case WifiSelectionState::NETWORK_LIST:
      renderNetworkList(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTED:
      renderConnected(&screen, &metrics);
      break;
    case WifiSelectionState::SAVE_PROMPT:
      renderSavePrompt(&screen, &metrics);
      break;
    case WifiSelectionState::CONNECTION_FAILED:
      renderConnectionFailed(&screen, &metrics);
      break;
    case WifiSelectionState::FORGET_PROMPT:
      renderForgetPrompt(&screen, &metrics);
      break;
    case WifiSelectionState::PASSWORD_ENTRY:
      // Unreachable: the keyboard is its own activity and render() returns
      // above while this state is set. Listed so the switch stays exhaustive.
      break;
  }

  renderer.displayBuffer();
}

void WifiSelectionActivity::renderNetworkList(const Rect* screen, const ThemeMetrics* metrics) const {
  const int contentTop =
      screen->y + metrics->topPadding + metrics->headerHeight + metrics->subHeaderHeight + metrics->verticalSpacing;
  const int contentHeight = screen->height - contentTop - metrics->verticalSpacing * 2;
  if (networks.empty()) {
    // No networks found or scan failed
    GUI.drawEmptyState(renderer, Rect{screen->x, contentTop, screen->width, contentHeight}, tr(STR_NO_NETWORKS),
                       tr(STR_PRESS_OK_SCAN), /*script=*/true);
  } else {
    GUI.drawList(
        renderer, Rect{screen->x, contentTop, screen->width, contentHeight}, static_cast<int>(networks.size()),
        selectedNetworkIndex, [this](int index) { return networks[index].ssid; }, nullptr, nullptr,
        [this](int index) {
          auto network = networks[index];
          return std::string(network.hasSavedPassword ? "+ " : "") + (network.isEncrypted ? "* " : "") +
                 getSignalStrengthIndicator(network.rssi);
        });
  }

  GUI.drawHelpText(renderer,
                   Rect{screen->x, screen->y + screen->height - metrics->contentSidePadding - 15, screen->width, 20},
                   tr(STR_NETWORK_LEGEND));

  if (networks.empty()) {
    // Confirm rescans here: "Connect" with nothing to connect to was a lie.
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const bool hasSavedPassword = networks[selectedNetworkIndex].hasSavedPassword;
  const char* forgetLabel = hasSavedPassword ? tr(STR_FORGET_BUTTON) : "";

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), forgetLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;

  if (state == WifiSelectionState::SCANNING) {
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, tr(STR_SCANNING));
  } else {
    UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_CONNECTING), true,
                              EpdFontFamily::BOLD);

    const std::string ssidInfo =
        renderer.truncatedText(UI_10_FONT_ID, (std::string(tr(STR_TO_PREFIX)) + selectedSSID).c_str(),
                               screen->width - metrics->contentSidePadding * 2);
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());
  }

  // Both states service Back in loop() now — say so. These were the only two
  // screens in the firmware with a completely empty legend bar.
  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnected(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 4) / 2;

  UITheme::drawCenteredText(renderer, *screen, SCRIPT_FONT_ID, top - 30, tr(STR_CONNECTED));

  const std::string ssidInfo =
      renderer.truncatedText(UI_10_FONT_ID, (std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID).c_str(),
                             screen->width - metrics->contentSidePadding * 2);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 10, ssidInfo.c_str());

  const std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, ipInfo.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

namespace {
// Shared by the save/forget prompts: centres the question (wrapped — several
// locales are wider than the panel) and draws the two bracket options sized
// from the measured labels, so long translations no longer overlap.
int drawPromptQuestion(const GfxRenderer& renderer, const Rect& screen, const int top, const char* question) {
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = top;
  for (const auto& line :
       renderer.wrappedText(UI_10_FONT_ID, question, screen.width - BaseMetrics::values.contentSidePadding * 2, 3)) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, line.c_str());
    y += lineHeight;
  }
  return y;
}

void drawPromptOptions(const GfxRenderer& renderer, const Rect& screen, const int buttonY, const char* first,
                       const char* second, const int selection) {
  const std::string firstSel = "[" + std::string(first) + "]";
  const std::string secondSel = "[" + std::string(second) + "]";
  const int firstW = renderer.getTextWidth(UI_10_FONT_ID, firstSel.c_str(), EpdFontFamily::BOLD);
  const int secondW = renderer.getTextWidth(UI_10_FONT_ID, secondSel.c_str(), EpdFontFamily::BOLD);
  const int spacing = 30;
  const int startX = screen.x + std::max(0, (screen.width - firstW - secondW - spacing) / 2);
  renderer.drawText(UI_10_FONT_ID, startX, buttonY, selection == 0 ? firstSel.c_str() : first, true,
                    selection == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, startX + firstW + spacing, buttonY, selection == 1 ? secondSel.c_str() : second,
                    true, selection == 1 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}
}  // namespace

void WifiSelectionActivity::renderSavePrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, SCRIPT_FONT_ID, top - 40, tr(STR_CONNECTED));

  const std::string ssidInfo =
      renderer.truncatedText(UI_10_FONT_ID, (std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID).c_str(),
                             screen->width - metrics->contentSidePadding * 2);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());

  const int questionBottom = drawPromptQuestion(renderer, *screen, top + 40, tr(STR_SAVE_PASSWORD));
  drawPromptOptions(renderer, *screen, questionBottom + metrics->verticalSpacing * 2, tr(STR_YES), tr(STR_NO),
                    savePromptSelection);

  // Back proceeds without saving — "Cancel" implied it would stop the
  // connection, so the slot stays unnamed.
  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderConnectionFailed(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, connectionError.c_str());

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_FORGET_NETWORK), true,
                            EpdFontFamily::BOLD);

  const std::string ssidInfo =
      renderer.truncatedText(UI_10_FONT_ID, (std::string(tr(STR_NETWORK_PREFIX)) + selectedSSID).c_str(),
                             screen->width - metrics->contentSidePadding * 2);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, ssidInfo.c_str());

  const int questionBottom = drawPromptQuestion(renderer, *screen, top + 40, tr(STR_FORGET_AND_REMOVE));
  drawPromptOptions(renderer, *screen, questionBottom + metrics->verticalSpacing * 2, tr(STR_CANCEL),
                    tr(STR_FORGET_BUTTON), forgetPromptSelection);

  // Use centralized button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WifiSelectionActivity::returnToNetworkList() {
  if (networks.empty()) {
    startWifiScan();
    return;
  }
  state = WifiSelectionState::NETWORK_LIST;
  requestUpdate();
}

void WifiSelectionActivity::onComplete(const bool connected) {
  ActivityResult result;
  result.isCancelled = !connected;
  if (connected) {
    ACHIEVEMENTS.record(AchievementEvent::WifiConnected);
    result.data = WifiResult{true, selectedSSID, connectedIP};
  }
  setResult(std::move(result));
  finish();
}
