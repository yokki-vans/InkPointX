#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "activities/Activity.h"
#include "util/HoldGestures.h"

class LibraryActivity final : public Activity {
 public:
  enum class Mode { AllBooks, Favorites };

  explicit LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::AllBooks)
      : Activity(mode == Mode::AllBooks ? "Library" : "Favorites", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static bool invalidateIndex();

 private:
  struct BookEntry {
    uint32_t pathOffset = 0;
    uint32_t titleOffset = 0;
    uint32_t authorOffset = 0;
    int recentRank = 1000;
    int16_t recentMetadataIndex = -1;
    int16_t favoriteMetadataIndex = -1;
    uint8_t percent = 0;
    bool favorite = false;
    bool isNew = true;
    bool progressLoaded = false;
  };

  enum class SortMode : uint8_t { Title, Author, Format, Recent, Count };

  static constexpr size_t MAX_LIBRARY_BOOKS = 1200;
  static constexpr size_t MAX_STRING_POOL_BYTES = 192 * 1024;
  static constexpr size_t NAME_BUFFER_SIZE = 384;
  static constexpr const char* INDEX_PATH = "/.crosspoint/library.idx";
  static constexpr const char* INDEX_TEMP_PATH = "/.crosspoint/library.idx.tmp";
  static constexpr const char* SCAN_SPOOL_PATH = "/.crosspoint/library.scan.tmp";
  // Acting on the selected book, so the shared short hold.
  static constexpr unsigned long FAVORITE_HOLD_MS = HoldGestures::SHORT_MS;

  Mode mode;
  std::unique_ptr<BookEntry[]> books;
  size_t bookCount = 0;
  std::unique_ptr<char[]> stringPool;
  size_t stringPoolSize = 0;
  std::unique_ptr<char[]> fileNameBuffer;
  size_t selectedIndex = 0;
  SortMode sortMode = SortMode::Title;
  bool longPressFired = false;
  bool loading = false;
  bool pendingTruncatedWarning = false;

  void loadBooks();
  void scanAllBooks();
  void loadFavorites();
  bool loadIndex();
  bool loadScanSpool(size_t count, size_t poolBytes);
  bool saveIndex(bool truncated) const;
  void clearCatalog();
  void refreshRuntimeMetadata();
  void ensureProgress(size_t index);
  std::string_view poolString(uint32_t offset) const;
  std::string_view bookPath(size_t index) const;
  std::string_view bookTitle(size_t index) const;
  std::string_view bookAuthor(size_t index) const;
  std::string bookSubtitle(size_t index);
  void sortBooks(int direction = 0);
  void toggleSelectedFavorite();
  const char* sortModeLabel() const;
};
