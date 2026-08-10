#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <array>
#include <memory>
#include <string>

#include "PairingCredentials.h"

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    HalFile file;
    String fileName;
    String path = "/";
    String finalPath;
    String tempPath;
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and avoid watchdog issues
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    std::array<uint8_t, UPLOAD_BUFFER_SIZE> buffer{};
    size_t bufferPos = 0;
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  WsUploadStatus getWsUploadStatus() const;

  // Get the port number
  uint16_t getPort() const { return port; }

  // Short, unambiguous per-server-session code intended for manual entry. The
  // first successful request exchanges it for a longer HttpOnly session
  // cookie used by the web UI and WebSocket connection.
  const char* getPairingCode() const { return pairingCode.data(); }

  // Authentication is captured when this server session is created. This
  // keeps the QR/address shown by the activity consistent for its lifetime.
  bool isAuthenticationEnabled() const { return authenticationEnabled; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  bool authenticationEnabled = true;
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;
  std::array<char, PairingCredentials::SESSION_TOKEN_LENGTH + 1> sessionToken{};
  std::array<char, PairingCredentials::CODE_LENGTH + 1> pairingCode{};

  void generatePairingCredentials();
  bool isAuthorizedRequest(WebServer& request) const;
  void rejectUnauthorized(WebServer& request) const;

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

  // File scanning
  void scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleFontUpload();
  void handleFontUploadData();
  void handleFontDelete();

  // Font upload state
  struct FontUploadState {
    HalFile file;
    std::string familyName;
    std::string filePath;
    std::string tempPath;
    bool valid = false;
    bool magicChecked = false;
    uint8_t magic[8] = {};
    size_t magicSize = 0;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::array<uint8_t, BUFFER_SIZE> buffer{};
    size_t bufferPos = 0;
  } fontUpload;

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();
};
