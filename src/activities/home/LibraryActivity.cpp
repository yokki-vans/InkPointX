#include "LibraryActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "FavoriteBooksStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/ProgressFile.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr char LIBRARY_INDEX_MAGIC[8] = {'I', 'P', 'X', 'L', 'I', 'B', '0', '1'};
constexpr uint16_t LIBRARY_INDEX_VERSION = 1;

#pragma pack(push, 1)
struct LibraryIndexHeader {
  char magic[8];
  uint16_t version;
  uint16_t recordSize;
  uint32_t bookCount;
  uint32_t poolBytes;
  uint32_t checksum;
  uint8_t truncated;
  uint8_t reserved[7];
};

struct LibraryDiskEntry {
  uint32_t pathOffset;
  uint32_t titleOffset;
  uint32_t authorOffset;
};

struct LibrarySpoolRecord {
  uint16_t pathBytes;
  uint16_t titleBytes;
  uint16_t authorBytes;
};
#pragma pack(pop)

uint32_t fnv1aUpdate(uint32_t hash, const void* data, const size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

bool writeAll(HalFile& file, const void* data, const size_t size) { return file.write(data, size) == size; }

bool writeSpoolString(HalFile& file, const std::string_view value) {
  return value.empty() || writeAll(file, value.data(), value.size());
}

bool isSupportedBook(const std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasFb2Extension(filename) ||
         FsHelpers::hasPdfExtension(filename);
}

std::string displayTitleFromPath(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
  const auto dot = path.find_last_of('.');
  const size_t nameEnd = dot == std::string::npos || dot < nameStart ? path.size() : dot;
  return path.substr(nameStart, nameEnd - nameStart);
}

std::string_view displayFormatFromPath(const std::string_view path) {
  const auto dot = path.find_last_of('.');
  return dot == std::string_view::npos || dot + 1 >= path.size() ? std::string_view{} : path.substr(dot + 1);
}

const RecentBook* findMetadata(const std::vector<RecentBook>& books, const std::string_view path,
                               int* index = nullptr) {
  for (size_t i = 0; i < books.size(); i++) {
    if (std::string_view(books[i].path) == path) {
      if (index) *index = static_cast<int>(i);
      return &books[i];
    }
  }
  return nullptr;
}

struct LibraryProgress {
  bool opened = false;
  uint8_t percent = 0;
};

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

LibraryProgress loadLibraryProgress(const std::string& path, const bool listedInRecents) {
  LibraryProgress result{listedInRecents, 0};
  const std::string cachePath = getBookCachePath(path);
  if (cachePath.empty()) return result;

  const std::string progressPath = cachePath + "/progress.bin";
  if (!Storage.exists(progressPath.c_str())) return result;
  HalFile progressFile;
  if (!Storage.openFileForRead("LIB", progressPath, progressFile)) return result;

  result.opened = true;
  uint8_t data[7]{};
  const int bytesRead = progressFile.read(data, sizeof(data));
  progressFile.close();
  if (bytesRead == 7 && data[6] <= 100) {
    result.percent = data[6];
    return result;
  }
  if (bytesRead != 4 && bytesRead != 6) return result;

  // One-time migration for progress written by older firmware. Loading an
  // existing book.bin is cheap and never starts indexing (buildIfMissing=false).
  // Once calculated, the seventh byte makes every later library visit O(1).
  Epub epub(path, "/.crosspoint");
  if (!epub.load(false, true)) return result;

  const uint16_t spineIndex = readLe16(data);
  if (spineIndex >= epub.getSpineItemsCount()) return result;
  uint16_t pageNumber = readLe16(data + 2);
  if (pageNumber == UINT16_MAX) pageNumber = 0;
  const uint16_t pageCount = bytesRead == 6 ? readLe16(data + 4) : 0;
  const float chapterProgress = pageCount > 0 ? std::min(1.0f, static_cast<float>(pageNumber) / pageCount) : 0.0f;
  const float bookProgress = std::clamp(epub.calculateProgress(spineIndex, chapterProgress), 0.0f, 1.0f);
  result.percent = static_cast<uint8_t>(bookProgress * 100.0f + 0.5f);

  data[6] = result.percent;
  if (!ProgressFile::writeAtomic(cachePath, data, sizeof(data))) {
    LOG_ERR("LIB", "Could not upgrade progress summary for %s", path.c_str());
  }
  return result;
}

std::string makeBookSubtitle(const std::string& author, const uint8_t percent) {
  if (percent == 0) return author;
  if (author.empty()) return std::to_string(percent) + "%";
  return author + " · " + std::to_string(percent) + "%";
}
}  // namespace

void LibraryActivity::clearCatalog() {
  books.reset();
  stringPool.reset();
  bookCount = 0;
  stringPoolSize = 0;
}

std::string_view LibraryActivity::poolString(const uint32_t offset) const {
  if (!stringPool || offset >= stringPoolSize) return {};
  const char* start = stringPool.get() + offset;
  const size_t remaining = stringPoolSize - offset;
  const void* terminator = memchr(start, '\0', remaining);
  if (!terminator) return {};
  return {start, static_cast<size_t>(static_cast<const char*>(terminator) - start)};
}

std::string_view LibraryActivity::bookPath(const size_t index) const {
  return index < bookCount ? poolString(books[index].pathOffset) : std::string_view{};
}

std::string_view LibraryActivity::bookTitle(const size_t index) const {
  if (index >= bookCount) return {};
  const BookEntry& book = books[index];
  const auto& recents = RECENT_BOOKS.getBooks();
  if (book.recentMetadataIndex >= 0 && static_cast<size_t>(book.recentMetadataIndex) < recents.size() &&
      !recents[book.recentMetadataIndex].title.empty()) {
    return recents[book.recentMetadataIndex].title;
  }
  const auto& favorites = FAVORITE_BOOKS.getBooks();
  if (book.favoriteMetadataIndex >= 0 && static_cast<size_t>(book.favoriteMetadataIndex) < favorites.size() &&
      !favorites[book.favoriteMetadataIndex].title.empty()) {
    return favorites[book.favoriteMetadataIndex].title;
  }
  return poolString(book.titleOffset);
}

std::string_view LibraryActivity::bookAuthor(const size_t index) const {
  if (index >= bookCount) return {};
  const BookEntry& book = books[index];
  const auto& recents = RECENT_BOOKS.getBooks();
  if (book.recentMetadataIndex >= 0 && static_cast<size_t>(book.recentMetadataIndex) < recents.size() &&
      !recents[book.recentMetadataIndex].author.empty()) {
    return recents[book.recentMetadataIndex].author;
  }
  const auto& favorites = FAVORITE_BOOKS.getBooks();
  if (book.favoriteMetadataIndex >= 0 && static_cast<size_t>(book.favoriteMetadataIndex) < favorites.size() &&
      !favorites[book.favoriteMetadataIndex].author.empty()) {
    return favorites[book.favoriteMetadataIndex].author;
  }
  return poolString(book.authorOffset);
}

void LibraryActivity::ensureProgress(const size_t index) {
  if (index >= bookCount || books[index].progressLoaded) return;
  BookEntry& book = books[index];
  const std::string path(bookPath(index));
  const LibraryProgress progress = loadLibraryProgress(path, book.recentMetadataIndex >= 0);
  book.progressLoaded = true;
  book.percent = progress.percent;
  book.isNew = !progress.opened;
}

std::string LibraryActivity::bookSubtitle(const size_t index) {
  ensureProgress(index);
  const std::string author(bookAuthor(index));
  return index < bookCount ? makeBookSubtitle(author, books[index].percent) : author;
}

int LibraryActivity::visibleItemsPerPage() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(0, UITheme::getListContentBottom(renderer, bookCount > 0) - contentTop);
  return std::max(1, GUI.getListPageItems(contentHeight, true));
}

void LibraryActivity::moveByPage(const bool forward) {
  if (bookCount == 0) return;
  const int selected = static_cast<int>(selectedIndex);
  const int total = static_cast<int>(bookCount);
  const int pageItems = visibleItemsPerPage();
  selectedIndex = static_cast<size_t>(forward ? ButtonNavigator::nextPageIndex(selected, total, pageItems)
                                              : ButtonNavigator::previousPageIndex(selected, total, pageItems));
  requestUpdate();
}

void LibraryActivity::refreshRuntimeMetadata() {
  const auto& recents = RECENT_BOOKS.getBooks();
  const auto& favorites = FAVORITE_BOOKS.getBooks();
  for (size_t i = 0; i < bookCount; ++i) {
    BookEntry& book = books[i];
    int recentIndex = -1;
    int favoriteIndex = -1;
    const std::string_view path = bookPath(i);
    findMetadata(recents, path, &recentIndex);
    findMetadata(favorites, path, &favoriteIndex);
    book.recentMetadataIndex = static_cast<int16_t>(recentIndex);
    book.favoriteMetadataIndex = static_cast<int16_t>(favoriteIndex);
    book.recentRank = recentIndex < 0 ? 1000 : recentIndex;
    book.favorite = favoriteIndex >= 0;
    book.isNew = recentIndex < 0;
    book.progressLoaded = false;
    book.percent = 0;
  }
}

bool LibraryActivity::loadIndex() {
  clearCatalog();
  HalFile file;
  if (!Storage.openFileForRead("LIB", INDEX_PATH, file)) return false;
  LibraryIndexHeader header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      memcmp(header.magic, LIBRARY_INDEX_MAGIC, sizeof(header.magic)) != 0 || header.version != LIBRARY_INDEX_VERSION ||
      header.recordSize != sizeof(LibraryDiskEntry) || header.bookCount > MAX_LIBRARY_BOOKS ||
      header.poolBytes > MAX_STRING_POOL_BYTES) {
    return false;
  }
  if (header.bookCount == 0) {
    pendingTruncatedWarning = header.truncated != 0;
    return true;
  }

  books = makeUniqueNoThrow<BookEntry[]>(header.bookCount);
  stringPool = makeUniqueNoThrow<char[]>(header.poolBytes);
  if (!books || !stringPool) {
    LOG_ERR("LIB", "OOM loading index: books=%u pool=%u", header.bookCount, header.poolBytes);
    clearCatalog();
    return false;
  }

  uint32_t checksum = 2166136261u;
  for (size_t i = 0; i < header.bookCount; ++i) {
    LibraryDiskEntry disk{};
    if (file.read(&disk, sizeof(disk)) != static_cast<int>(sizeof(disk))) {
      clearCatalog();
      return false;
    }
    checksum = fnv1aUpdate(checksum, &disk, sizeof(disk));
    books[i].pathOffset = disk.pathOffset;
    books[i].titleOffset = disk.titleOffset;
    books[i].authorOffset = disk.authorOffset;
  }
  if (header.poolBytes > 0 && file.read(stringPool.get(), header.poolBytes) != static_cast<int>(header.poolBytes)) {
    clearCatalog();
    return false;
  }
  checksum = fnv1aUpdate(checksum, stringPool.get(), header.poolBytes);
  bookCount = header.bookCount;
  stringPoolSize = header.poolBytes;
  if (checksum != header.checksum) {
    LOG_ERR("LIB", "Library index checksum mismatch");
    clearCatalog();
    return false;
  }
  for (size_t i = 0; i < bookCount; ++i) {
    if (poolString(books[i].pathOffset).empty() || poolString(books[i].titleOffset).empty() ||
        books[i].authorOffset >= stringPoolSize) {
      clearCatalog();
      return false;
    }
  }
  pendingTruncatedWarning = header.truncated != 0;
  return true;
}

bool LibraryActivity::saveIndex(const bool truncated) const {
  HalFile file;
  if (!Storage.openFileForWrite("LIB", INDEX_TEMP_PATH, file)) return false;
  uint32_t checksum = 2166136261u;
  for (size_t i = 0; i < bookCount; ++i) {
    const LibraryDiskEntry disk{books[i].pathOffset, books[i].titleOffset, books[i].authorOffset};
    checksum = fnv1aUpdate(checksum, &disk, sizeof(disk));
  }
  checksum = fnv1aUpdate(checksum, stringPool.get(), stringPoolSize);
  LibraryIndexHeader header{};
  memcpy(header.magic, LIBRARY_INDEX_MAGIC, sizeof(header.magic));
  header.version = LIBRARY_INDEX_VERSION;
  header.recordSize = sizeof(LibraryDiskEntry);
  header.bookCount = static_cast<uint32_t>(bookCount);
  header.poolBytes = static_cast<uint32_t>(stringPoolSize);
  header.checksum = checksum;
  header.truncated = truncated ? 1 : 0;

  bool ok = writeAll(file, &header, sizeof(header));
  for (size_t i = 0; ok && i < bookCount; ++i) {
    const LibraryDiskEntry disk{books[i].pathOffset, books[i].titleOffset, books[i].authorOffset};
    ok = writeAll(file, &disk, sizeof(disk));
  }
  if (ok && stringPoolSize > 0) ok = writeAll(file, stringPool.get(), stringPoolSize);
  file.close();
  if (!ok || !Storage.replaceFileFromTemp(INDEX_PATH, INDEX_TEMP_PATH)) {
    Storage.remove(INDEX_TEMP_PATH);
    return false;
  }
  return true;
}

bool LibraryActivity::loadScanSpool(const size_t count, const size_t poolBytes) {
  clearCatalog();
  if (count == 0) return true;
  books = makeUniqueNoThrow<BookEntry[]>(count);
  stringPool = makeUniqueNoThrow<char[]>(poolBytes);
  if (!books || !stringPool) {
    LOG_ERR("LIB", "OOM materialising scan: books=%u pool=%u", static_cast<unsigned>(count),
            static_cast<unsigned>(poolBytes));
    clearCatalog();
    return false;
  }
  HalFile spool;
  if (!Storage.openFileForRead("LIB", SCAN_SPOOL_PATH, spool)) {
    clearCatalog();
    return false;
  }
  size_t cursor = 0;
  for (size_t i = 0; i < count; ++i) {
    LibrarySpoolRecord record{};
    if (spool.read(&record, sizeof(record)) != static_cast<int>(sizeof(record))) {
      clearCatalog();
      return false;
    }
    const std::array<uint16_t, 3> lengths{record.pathBytes, record.titleBytes, record.authorBytes};
    uint32_t* offsets[] = {&books[i].pathOffset, &books[i].titleOffset, &books[i].authorOffset};
    for (size_t field = 0; field < lengths.size(); ++field) {
      const size_t length = lengths[field];
      if (cursor + length + 1 > poolBytes ||
          (length > 0 && spool.read(stringPool.get() + cursor, length) != static_cast<int>(length))) {
        clearCatalog();
        return false;
      }
      *offsets[field] = static_cast<uint32_t>(cursor);
      cursor += length;
      stringPool[cursor++] = '\0';
    }
  }
  bookCount = count;
  stringPoolSize = cursor;
  Storage.remove(SCAN_SPOOL_PATH);
  return cursor == poolBytes;
}

bool LibraryActivity::invalidateIndex() {
  Storage.remove(INDEX_TEMP_PATH);
  Storage.remove(SCAN_SPOOL_PATH);
  return !Storage.exists(INDEX_PATH) || Storage.remove(INDEX_PATH);
}

void LibraryActivity::onEnter() {
  Activity::onEnter();
  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("LIB", "OOM: filename buffer");
    requestUpdate();
    return;
  }

  FAVORITE_BOOKS.pruneMissing();
  if (mode == Mode::AllBooks && loadIndex()) {
    refreshRuntimeMetadata();
    sortBooks();
    loading = false;
    selectedIndex = 0;
    requestUpdate();
    return;
  }

  loading = true;
  requestUpdateAndWait();
  loadBooks();
  loading = false;
  selectedIndex = 0;
  requestUpdate();
}

void LibraryActivity::onExit() {
  Activity::onExit();
  clearCatalog();
  fileNameBuffer.reset();
}

void LibraryActivity::loadBooks() {
  clearCatalog();
  if (mode == Mode::Favorites)
    loadFavorites();
  else
    scanAllBooks();
  refreshRuntimeMetadata();
  sortBooks();
}

void LibraryActivity::loadFavorites() {
  const auto& favorites = FAVORITE_BOOKS.getBooks();
  const size_t count = std::min(favorites.size(), MAX_LIBRARY_BOOKS);
  size_t poolBytes = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto& favorite = favorites[i];
    const std::string title = favorite.title.empty() ? displayTitleFromPath(favorite.path) : favorite.title;
    poolBytes += favorite.path.size() + title.size() + favorite.author.size() + 3;
  }
  if (poolBytes > MAX_STRING_POOL_BYTES) return;
  books = makeUniqueNoThrow<BookEntry[]>(count);
  stringPool = makeUniqueNoThrow<char[]>(poolBytes);
  if ((count > 0 && !books) || (poolBytes > 0 && !stringPool)) {
    clearCatalog();
    return;
  }
  size_t cursor = 0;
  const auto append = [this, &cursor](const std::string_view value) {
    const uint32_t offset = static_cast<uint32_t>(cursor);
    if (!value.empty()) memcpy(stringPool.get() + cursor, value.data(), value.size());
    cursor += value.size();
    stringPool[cursor++] = '\0';
    return offset;
  };
  for (size_t i = 0; i < count; ++i) {
    const auto& favorite = favorites[i];
    const std::string title = favorite.title.empty() ? displayTitleFromPath(favorite.path) : favorite.title;
    books[i].pathOffset = append(favorite.path);
    books[i].titleOffset = append(title);
    books[i].authorOffset = append(favorite.author);
  }
  bookCount = count;
  stringPoolSize = cursor;
}

void LibraryActivity::scanAllBooks() {
  if (!fileNameBuffer) return;
  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.remove(SCAN_SPOOL_PATH);
  HalFile spool;
  if (!Storage.openFileForWrite("LIB", SCAN_SPOOL_PATH, spool)) return;

  std::vector<std::string> directories;
  directories.reserve(64);
  directories.emplace_back("/");
  constexpr size_t MAX_SCANNED_DIRECTORIES = 256;
  constexpr size_t MAX_SCANNED_ENTRIES = 16384;
  size_t scannedDirectories = 0;
  size_t scannedEntries = 0;
  size_t count = 0;
  size_t poolBytes = 0;
  bool truncated = false;
  bool writeFailed = false;

  const auto& recents = RECENT_BOOKS.getBooks();
  const auto& favorites = FAVORITE_BOOKS.getBooks();

  while (!directories.empty() && scannedDirectories < MAX_SCANNED_DIRECTORIES && scannedEntries < MAX_SCANNED_ENTRIES &&
         !truncated && !writeFailed) {
    std::string directory = std::move(directories.back());
    directories.pop_back();
    ++scannedDirectories;

    auto root = Storage.open(directory.c_str());
    if (!root || !root.isDirectory()) continue;
    root.rewindDirectory();

    for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (++scannedEntries > MAX_SCANNED_ENTRIES) {
        truncated = true;
        break;
      }
      if ((scannedEntries & 0x0F) == 0) {
        esp_task_wdt_reset();
        yield();
      }
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      const char* name = fileNameBuffer.get();
      if (name[0] == '\0' || strcmp(name, "System Volume Information") == 0) continue;
      if (name[0] == '.' && (!SETTINGS.showHiddenFiles || strcmp(name, ".crosspoint") == 0)) continue;

      std::string fullPath = directory;
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += name;

      if (entry.isDirectory()) {
        if (directories.size() < MAX_SCANNED_DIRECTORIES)
          directories.push_back(std::move(fullPath));
        else
          truncated = true;
        continue;
      }
      if (!isSupportedBook(name)) continue;

      const RecentBook* recent = findMetadata(recents, fullPath);
      const RecentBook* favorite = findMetadata(favorites, fullPath);
      const std::string title = recent && !recent->title.empty()       ? recent->title
                                : favorite && !favorite->title.empty() ? favorite->title
                                                                       : displayTitleFromPath(fullPath);
      const std::string_view author = recent && !recent->author.empty()       ? std::string_view(recent->author)
                                      : favorite && !favorite->author.empty() ? std::string_view(favorite->author)
                                                                              : std::string_view{};
      if (fullPath.size() > UINT16_MAX || title.size() > UINT16_MAX || author.size() > UINT16_MAX ||
          count >= MAX_LIBRARY_BOOKS ||
          poolBytes + fullPath.size() + title.size() + author.size() + 3 > MAX_STRING_POOL_BYTES) {
        truncated = true;
        break;
      }
      const LibrarySpoolRecord record{static_cast<uint16_t>(fullPath.size()), static_cast<uint16_t>(title.size()),
                                      static_cast<uint16_t>(author.size())};
      writeFailed = !writeAll(spool, &record, sizeof(record)) || !writeSpoolString(spool, fullPath) ||
                    !writeSpoolString(spool, title) || !writeSpoolString(spool, author);
      if (writeFailed) break;
      poolBytes += fullPath.size() + title.size() + author.size() + 3;
      ++count;
    }
  }
  if (!directories.empty()) truncated = true;
  spool.close();
  if (writeFailed || !loadScanSpool(count, poolBytes)) {
    Storage.remove(SCAN_SPOOL_PATH);
    clearCatalog();
    return;
  }
  pendingTruncatedWarning = truncated;
  if (!saveIndex(truncated)) {
    LOG_ERR("LIB", "Could not persist library index");
  }
}

const char* LibraryActivity::sortModeLabel() const {
  switch (sortMode) {
    case SortMode::Title:
      return tr(STR_SORT_TITLE);
    case SortMode::Author:
      return tr(STR_SORT_AUTHOR);
    case SortMode::Format:
      return tr(STR_SORT_FORMAT);
    case SortMode::Recent:
      return tr(STR_SORT_RECENT);
    case SortMode::Count:
      break;
  }
  return tr(STR_SORT_TITLE);
}

void LibraryActivity::sortBooks(const int direction) {
  if (direction != 0) {
    const int count = static_cast<int>(SortMode::Count);
    int value = static_cast<int>(sortMode);
    value = (value + direction + count) % count;
    sortMode = static_cast<SortMode>(value);
  }

  if (bookCount < 2) {
    selectedIndex = 0;
    return;
  }

  const auto titleOf = [this](const BookEntry& entry) -> std::string_view {
    const auto& recents = RECENT_BOOKS.getBooks();
    if (entry.recentMetadataIndex >= 0 && static_cast<size_t>(entry.recentMetadataIndex) < recents.size() &&
        !recents[entry.recentMetadataIndex].title.empty()) {
      return recents[entry.recentMetadataIndex].title;
    }
    const auto& favorites = FAVORITE_BOOKS.getBooks();
    if (entry.favoriteMetadataIndex >= 0 && static_cast<size_t>(entry.favoriteMetadataIndex) < favorites.size() &&
        !favorites[entry.favoriteMetadataIndex].title.empty()) {
      return favorites[entry.favoriteMetadataIndex].title;
    }
    return poolString(entry.titleOffset);
  };
  const auto authorOf = [this](const BookEntry& entry) -> std::string_view {
    const auto& recents = RECENT_BOOKS.getBooks();
    if (entry.recentMetadataIndex >= 0 && static_cast<size_t>(entry.recentMetadataIndex) < recents.size() &&
        !recents[entry.recentMetadataIndex].author.empty()) {
      return recents[entry.recentMetadataIndex].author;
    }
    const auto& favorites = FAVORITE_BOOKS.getBooks();
    if (entry.favoriteMetadataIndex >= 0 && static_cast<size_t>(entry.favoriteMetadataIndex) < favorites.size() &&
        !favorites[entry.favoriteMetadataIndex].author.empty()) {
      return favorites[entry.favoriteMetadataIndex].author;
    }
    return poolString(entry.authorOffset);
  };
  std::sort(books.get(), books.get() + bookCount, [&](const BookEntry& left, const BookEntry& right) {
    switch (sortMode) {
      case SortMode::Author:
        if (authorOf(left).empty() != authorOf(right).empty()) return !authorOf(left).empty();
        if (authorOf(left) != authorOf(right)) return authorOf(left) < authorOf(right);
        break;
      case SortMode::Format:
        if (displayFormatFromPath(poolString(left.pathOffset)) != displayFormatFromPath(poolString(right.pathOffset))) {
          return displayFormatFromPath(poolString(left.pathOffset)) <
                 displayFormatFromPath(poolString(right.pathOffset));
        }
        break;
      case SortMode::Recent:
        if (left.recentRank != right.recentRank) return left.recentRank < right.recentRank;
        break;
      case SortMode::Title:
      case SortMode::Count:
        break;
    }
    return titleOf(left) < titleOf(right);
  });
  selectedIndex = std::min(selectedIndex, bookCount == 0 ? size_t{0} : bookCount - 1);
}

void LibraryActivity::toggleSelectedFavorite() {
  if (selectedIndex >= bookCount) return;
  BookEntry& book = books[selectedIndex];
  const std::string path(bookPath(selectedIndex));
  const std::string title(bookTitle(selectedIndex));
  const std::string author(bookAuthor(selectedIndex));
  if (!FAVORITE_BOOKS.toggle(path, title, author)) {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    renderer.displayBuffer();
    return;
  }

  if (mode == Mode::Favorites) {
    for (size_t i = selectedIndex + 1; i < bookCount; ++i) books[i - 1] = books[i];
    --bookCount;
    selectedIndex = std::min(selectedIndex, bookCount == 0 ? size_t{0} : bookCount - 1);
  } else {
    book.favorite = !book.favorite;
    refreshRuntimeMetadata();
  }
  requestUpdate(true);
}

void LibraryActivity::loop() {
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;
    return;
  }

  if (bookCount > 0 && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= FAVORITE_HOLD_MS) {
    longPressFired = true;
    toggleSelectedFavorite();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < bookCount) onSelectBook(std::string(bookPath(selectedIndex)));
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(mode == Mode::Favorites ? HomeMenuItem::FAVORITES : HomeMenuItem::LIBRARY);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down) && bookCount > 0) {
    selectedIndex = (selectedIndex + 1) % bookCount;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) && bookCount > 0) {
    selectedIndex = (selectedIndex + bookCount - 1) % bookCount;
    requestUpdate();
  }

  // A short press remains one row. Holding either side button advances by a
  // whole visible page every 500 ms, which keeps large catalogues practical
  // without introducing a separate paging mode.
  pageNavigator.onNextContinuous([this] { moveByPage(true); });
  pageNavigator.onPreviousContinuous([this] { moveByPage(false); });

  if (mode == Mode::AllBooks && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    sortBooks(1);
    requestUpdate();
  } else if (mode == Mode::AllBooks && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    sortBooks(-1);
    requestUpdate();
  }
}

void LibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const char* title = mode == Mode::Favorites ? tr(STR_FAVORITES) : tr(STR_BOOKS);

  std::string sortValue;
  // The mode name alone. At the larger type scale "Sorting: Recently opened" plus
  // the screen title plus the battery does not fit 480 px, and the prefix is the
  // half the user already knows -- it was the value that got truncated away.
  if (mode == Mode::AllBooks) sortValue = sortModeLabel();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title,
                 sortValue.empty() ? nullptr : sortValue.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(0, UITheme::getListContentBottom(renderer, bookCount > 0) - contentTop);
  if (bookCount == 0) {
    GUI.drawEmptyState(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        loading ? tr(STR_SCANNING) : (mode == Mode::Favorites ? tr(STR_NO_FAVORITES) : tr(STR_NO_FILES_FOUND)), nullptr,
        /*script=*/true);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(bookCount),
        static_cast<int>(selectedIndex), [this](int index) { return std::string(bookTitle(index)); },
        [this](int index) { return bookSubtitle(index); },
        [this](int index) {
          ensureProgress(index);
          return books[index].isNew ? UIIcon::BookNew : UITheme::getFileIcon(std::string(bookPath(index)));
        },
        // No format column. It cost roughly 70 px of every row to repeat what the
        // leading icon already says -- this is a book -- while the title, which is
        // how anyone actually picks what to read, was truncated to make room. The
        // format is still on the row in Files and on the Properties screen, where
        // it is the point rather than decoration.
        nullptr, false, nullptr,
        // The favourite marker is a real accessory. It used to be a U+2605 star
        // appended to the format string, but the UI font has no glyph for it, so
        // nothing was drawn at all and the two padding spaces left favourited rows
        // with a ragged right edge.
        [this](int index) { return books[index].favorite ? UIAccessory::Favorite : UIAccessory::None; });
    GUI.drawFooterCounter(renderer, static_cast<int>(selectedIndex), static_cast<int>(bookCount));
  }

  if (pendingTruncatedWarning) {
    GUI.drawPopup(renderer, tr(STR_LIBRARY_PARTIAL));
    pendingTruncatedWarning = false;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), bookCount == 0 ? "" : tr(STR_OPEN),
                                            mode == Mode::AllBooks ? "<" : "", mode == Mode::AllBooks ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
