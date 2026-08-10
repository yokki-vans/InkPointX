#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using String = std::string;

class HalFile {
  friend class HalStorage;
  std::filesystem::path path_;
  mutable std::fstream stream_;

  explicit HalFile(const std::filesystem::path& path, const bool write = false)
      : path_(path),
        stream_(path, write ? (std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc)
                            : (std::ios::binary | std::ios::in)) {}

 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  bool close() {
    if (stream_.is_open()) stream_.close();
    return true;
  }
  size_t fileSize() const { return std::filesystem::file_size(path_); }
  bool seekSet(size_t offset) {
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    return static_cast<bool>(stream_);
  }
  bool seek64(uint64_t offset) { return seekSet(static_cast<size_t>(offset)); }
  size_t position() const {
    const auto pos = stream_.tellg();
    return pos < 0 ? 0 : static_cast<size_t>(pos);
  }
  int read(void* buffer, size_t size) {
    stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return static_cast<int>(stream_.gcount());
  }
  size_t write(const void* buffer, size_t size) {
    stream_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    return stream_ ? size : 0;
  }
  explicit operator bool() const { return stream_.is_open(); }
};

class HalStorage {
  std::filesystem::path root_;

 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  void setRoot(std::filesystem::path root) { root_ = std::move(root); }
  std::filesystem::path resolve(const std::string& logical) const {
    std::string relative = logical;
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    return root_ / relative;
  }
  std::vector<String> listFiles(const char* logical) {
    std::vector<String> names;
    const auto directory = resolve(logical ? logical : "");
    if (!std::filesystem::is_directory(directory)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file()) names.push_back(entry.path().filename().string());
    }
    return names;
  }
  bool openFileForRead(const char*, const std::string& logical, HalFile& file) {
    file = HalFile(resolve(logical));
    return static_cast<bool>(file);
  }
  bool openFileForWrite(const char*, const std::string& logical, HalFile& file) {
    const auto path = resolve(logical);
    std::filesystem::create_directories(path.parent_path());
    file = HalFile(path, true);
    return static_cast<bool>(file);
  }
  bool replaceFileFromTemp(const char* logical, const char* tempLogical) {
    std::error_code error;
    const auto destination = resolve(logical ? logical : "");
    const auto temporary = resolve(tempLogical ? tempLogical : "");
    std::filesystem::remove(destination, error);
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    return !error;
  }
  bool remove(const char* logical) {
    std::error_code error;
    return std::filesystem::remove(resolve(logical ? logical : ""), error) && !error;
  }
};

#define Storage HalStorage::getInstance()
