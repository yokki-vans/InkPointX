#pragma once

#include <HalStorage.h>

#include <array>
#include <cstdint>
#include <string>

// Low-memory reader for an uncompressed StarDict set (.ifo + .idx + .dict).
// The complete .idx never lives in RAM: one checkpoint is retained for every
// 256 entries and lookups scan only the matching bracket.
class StarDictLookup {
 public:
  using ProgressCallback = void (*)(void* context, uint32_t completedBytes, uint32_t totalBytes);

  StarDictLookup() = default;
  ~StarDictLookup() { close(); }

  StarDictLookup(const StarDictLookup&) = delete;
  StarDictLookup& operator=(const StarDictLookup&) = delete;

  bool open(const std::string& folderPath);
  void close();
  bool isOpen() const { return isOpen_; }
  const std::string& bookname() const { return bookname_; }
  void setProgressCallback(ProgressCallback callback, void* context) {
    progressCallback_ = callback;
    progressContext_ = context;
  }

  static constexpr uint32_t MAX_DEFINITION_BYTES = 4000;
  bool lookup(const std::string& queryWord, std::string& outDefinition, bool* outTruncated = nullptr);

 private:
  static constexpr size_t MAX_CHECKPOINTS = 256;
  static constexpr size_t CHECKPOINT_WORD_BYTES = 64;

  struct Checkpoint {
    uint32_t idxOffset = 0;
    uint16_t fullLength = 0;
    char entryText[CHECKPOINT_WORD_BYTES]{};
  };

  bool parseIfo(const std::string& path);
  bool buildCheckpoints(const std::string& cachePath, uint32_t fingerprint);
  bool loadCheckpointCache(const std::string& cachePath, uint32_t fingerprint);
  void saveCheckpointCache(const std::string& cachePath, uint32_t fingerprint) const;
  uint32_t calculateIndexFingerprint();
  void assignCheckpoint(Checkpoint& checkpoint, uint32_t offset, const std::string& word);
  int compareCheckpoint(const Checkpoint& checkpoint, const std::string& candidate) const;
  bool readIdxEntryAt(uint32_t idxOffset, std::string& word, uint64_t& dictOffset, uint32_t& dictSize,
                      uint32_t& nextOffset);
  bool lookupViaCheckpoints(const std::string& candidate, uint64_t& dictOffset, uint32_t& dictSize);
  bool lookupViaLinearScan(const std::string& candidateLower, uint64_t& dictOffset, uint32_t& dictSize);
  bool decodeDefinitionData(const std::string& raw, std::string& decoded) const;

  bool isOpen_ = false;
  HalFile idxFile_;
  HalFile dictFile_;
  std::string bookname_;
  std::string sameTypeSequence_;
  uint32_t wordCount_ = 0;
  uint32_t idxFileSize_ = 0;
  uint64_t dictFileSize_ = 0;
  bool use64BitOffsets_ = false;
  bool indexSorted_ = true;
  uint32_t checkpointStride_ = 1;
  uint16_t checkpointCount_ = 0;
  std::array<Checkpoint, MAX_CHECKPOINTS> checkpoints_{};
  ProgressCallback progressCallback_ = nullptr;
  void* progressContext_ = nullptr;
};
