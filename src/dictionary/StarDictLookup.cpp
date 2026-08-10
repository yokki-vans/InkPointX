#include "StarDictLookup.h"

#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char CHECKPOINT_CACHE_MAGIC[8] = {'I', 'P', 'X', 'D', 'I', 'C', 'T', '1'};
constexpr uint16_t CHECKPOINT_CACHE_VERSION = 1;

#pragma pack(push, 1)
struct CheckpointCacheHeader {
  char magic[8];
  uint16_t version;
  uint16_t recordSize;
  uint32_t idxFileSize;
  uint64_t dictFileSize;
  uint32_t fingerprint;
  uint32_t wordCount;
  uint32_t stride;
  uint16_t checkpointCount;
  uint8_t sorted;
  uint8_t offsetBits;
};
#pragma pack(pop)

uint32_t fnv1a(const uint8_t* data, const size_t size, uint32_t hash = 2166136261u) {
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t readBe32(HalFile& file, bool& ok) {
  uint8_t b[4]{};
  ok = file.read(b, sizeof(b)) == static_cast<int>(sizeof(b));
  return ok ? (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                  (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3])
            : 0;
}

uint64_t readBe64(HalFile& file, bool& ok) {
  uint8_t b[8]{};
  ok = file.read(b, sizeof(b)) == static_cast<int>(sizeof(b));
  uint64_t value = 0;
  if (ok) {
    for (uint8_t byte : b) value = (value << 8) | byte;
  }
  return value;
}

bool readCString(HalFile& file, std::string& out) {
  out.clear();
  char c = 0;
  while (out.size() <= 256 && file.read(&c, 1) == 1) {
    if (c == '\0') return true;
    out.push_back(c);
  }
  return false;
}

bool endsWithIgnoreCase(const std::string& value, const char* suffix) {
  const size_t suffixLen = strlen(suffix);
  if (value.size() < suffixLen) return false;
  const size_t start = value.size() - suffixLen;
  for (size_t i = 0; i < suffixLen; ++i) {
    if (std::tolower(static_cast<unsigned char>(value[start + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

std::string lowerCopy(const std::string& input) {
  std::string out = input;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string titleCopy(const std::string& input) {
  std::string out = lowerCopy(input);
  if (!out.empty()) out.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(out.front())));
  return out;
}

// Keep UTF-8 bytes. ASCII punctuation is stripped, while non-ASCII leading and
// trailing bytes remain part of the word and can be looked up verbatim.
std::string stripPunctuation(const std::string& input) {
  size_t begin = 0;
  size_t end = input.size();
  const auto keep = [](unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '\'' || c == '-'; };
  while (begin < end && !keep(static_cast<unsigned char>(input[begin]))) ++begin;
  while (end > begin && !keep(static_cast<unsigned char>(input[end - 1]))) --end;
  return input.substr(begin, end - begin);
}

std::vector<std::string> stemCandidates(const std::string& lower) {
  std::vector<std::string> out;
  const size_t n = lower.size();
  const auto add = [&out](std::string value) {
    if (value.size() >= 2 && std::find(out.begin(), out.end(), value) == out.end()) out.push_back(std::move(value));
  };
  if (n > 2 && lower.compare(n - 2, 2, "'s") == 0) add(lower.substr(0, n - 2));
  if (n > 4 && lower.compare(n - 3, 3, "ing") == 0) {
    const std::string base = lower.substr(0, n - 3);
    add(base);
    add(base + "e");
    if (base.size() >= 3 && base[base.size() - 1] == base[base.size() - 2]) add(base.substr(0, base.size() - 1));
  }
  if (n > 3 && lower.compare(n - 3, 3, "ied") == 0) add(lower.substr(0, n - 3) + "y");
  if (n > 3 && lower.compare(n - 2, 2, "ed") == 0) {
    const std::string base = lower.substr(0, n - 2);
    add(base);
    add(base + "e");
    if (base.size() >= 3 && base[base.size() - 1] == base[base.size() - 2]) add(base.substr(0, base.size() - 1));
  }
  if (n > 4 && lower.compare(n - 3, 3, "ies") == 0) add(lower.substr(0, n - 3) + "y");
  if (n > 3 && lower.compare(n - 2, 2, "es") == 0) add(lower.substr(0, n - 2));
  if (n > 2 && lower.back() == 's' && lower[n - 2] != 's') add(lower.substr(0, n - 1));
  return out;
}

}  // namespace

void StarDictLookup::close() {
  // close() is intentionally safe before open(), after a partial open and on
  // repeated activity teardown.  Guarding the handles also documents that
  // these two files are independently acquired.
  if (idxFile_) idxFile_.close();
  if (dictFile_) dictFile_.close();
  checkpointCount_ = 0;
  checkpointStride_ = 1;
  indexSorted_ = true;
  std::string().swap(bookname_);
  std::string().swap(sameTypeSequence_);
  wordCount_ = 0;
  idxFileSize_ = 0;
  dictFileSize_ = 0;
  use64BitOffsets_ = false;
  isOpen_ = false;
}

bool StarDictLookup::parseIfo(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("DICT", path, file)) return false;
  // A valid .ifo is a short text header. Reject absurd input rather than
  // feeding a renamed book into an unbounded allocation.
  const size_t size = file.fileSize();
  if (size == 0 || size > 16384) return false;
  std::string contents(size, '\0');
  if (file.read(contents.data(), size) != static_cast<int>(size)) return false;
  size_t start = 0;
  while (start < contents.size()) {
    const size_t end = contents.find('\n', start);
    std::string line = contents.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? contents.size() : end + 1;
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    size_t first = 0;
    while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
    const size_t equals = line.find('=', first);
    if (equals == std::string::npos || equals == first) continue;
    const std::string key = line.substr(first, equals - first);
    std::string value = line.substr(equals + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    if (key == "bookname")
      bookname_ = value;
    else if (key == "wordcount")
      wordCount_ = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
    else if (key == "idxfilesize")
      idxFileSize_ = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
    else if (key == "sametypesequence")
      sameTypeSequence_ = value;
    else if (key == "idxoffsetbits")
      use64BitOffsets_ = strtoul(value.c_str(), nullptr, 10) == 64;
  }
  return true;
}

bool StarDictLookup::open(const std::string& folderPath) {
  close();
  std::string ifoPath;
  std::string idxPath;
  std::string dictPath;
  for (const auto& entry : Storage.listFiles(folderPath.c_str())) {
    const std::string name = entry.c_str();
    if (name.empty() || name.front() == '.') continue;
    const std::string full = folderPath + "/" + name;
    if (endsWithIgnoreCase(name, ".ifo"))
      ifoPath = full;
    else if (endsWithIgnoreCase(name, ".idx"))
      idxPath = full;
    else if (endsWithIgnoreCase(name, ".dict"))
      dictPath = full;
  }
  if (ifoPath.empty() || idxPath.empty() || dictPath.empty()) {
    LOG_ERR("DICT", "Missing .ifo/.idx/.dict in %s", folderPath.c_str());
    return false;
  }
  if (!parseIfo(ifoPath) || !Storage.openFileForRead("DICT", idxPath, idxFile_) ||
      !Storage.openFileForRead("DICT", dictPath, dictFile_)) {
    close();
    return false;
  }
  const size_t idxSize = idxFile_.fileSize();
  if (idxSize == 0 || idxSize > UINT32_MAX) {
    close();
    return false;
  }
  idxFileSize_ = static_cast<uint32_t>(idxSize);
  dictFileSize_ = dictFile_.fileSize();
  const uint32_t fingerprint = calculateIndexFingerprint();
  const std::string cachePath = folderPath + "/.inkpointx-dictionary.idx";
  if (!loadCheckpointCache(cachePath, fingerprint) && !buildCheckpoints(cachePath, fingerprint)) {
    close();
    return false;
  }
  isOpen_ = true;
  LOG_INF("DICT", "Opened %s: %u words, %u checkpoints (stride=%u, sorted=%u)", bookname_.c_str(), wordCount_,
          static_cast<unsigned>(checkpointCount_), checkpointStride_, indexSorted_ ? 1u : 0u);
  return true;
}

uint32_t StarDictLookup::calculateIndexFingerprint() {
  std::array<uint8_t, 128> sample{};
  uint32_t hash = fnv1a(reinterpret_cast<const uint8_t*>(&idxFileSize_), sizeof(idxFileSize_));
  const std::array<uint32_t, 3> starts = {
      0u,
      idxFileSize_ > sample.size() ? static_cast<uint32_t>((idxFileSize_ - sample.size()) / 2) : 0u,
      idxFileSize_ > sample.size() ? static_cast<uint32_t>(idxFileSize_ - sample.size()) : 0u,
  };
  for (const uint32_t start : starts) {
    if (!idxFile_.seekSet(start)) continue;
    const size_t wanted = std::min<size_t>(sample.size(), idxFileSize_ - start);
    const int read = idxFile_.read(sample.data(), wanted);
    if (read > 0) hash = fnv1a(sample.data(), static_cast<size_t>(read), hash);
  }
  idxFile_.seekSet(0);
  return hash;
}

void StarDictLookup::assignCheckpoint(Checkpoint& checkpoint, const uint32_t offset, const std::string& word) {
  checkpoint = {};
  checkpoint.idxOffset = offset;
  checkpoint.fullLength = static_cast<uint16_t>(std::min<size_t>(word.size(), UINT16_MAX));
  const size_t copy = std::min(word.size(), CHECKPOINT_WORD_BYTES - 1);
  if (copy > 0) memcpy(checkpoint.entryText, word.data(), copy);
  checkpoint.entryText[copy] = '\0';
}

int StarDictLookup::compareCheckpoint(const Checkpoint& checkpoint, const std::string& candidate) const {
  const size_t storedLength = std::min<size_t>(checkpoint.fullLength, CHECKPOINT_WORD_BYTES - 1);
  const size_t common = std::min(storedLength, candidate.size());
  const int compared = common == 0 ? 0 : memcmp(checkpoint.entryText, candidate.data(), common);
  if (compared != 0) return compared;
  if (checkpoint.fullLength < candidate.size()) return -1;
  if (checkpoint.fullLength > candidate.size()) return 1;
  return 0;
}

bool StarDictLookup::loadCheckpointCache(const std::string& cachePath, const uint32_t fingerprint) {
  HalFile cache;
  if (!Storage.openFileForRead("DICT", cachePath, cache)) return false;
  CheckpointCacheHeader header{};
  if (cache.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      memcmp(header.magic, CHECKPOINT_CACHE_MAGIC, sizeof(header.magic)) != 0 ||
      header.version != CHECKPOINT_CACHE_VERSION || header.recordSize != sizeof(Checkpoint) ||
      header.idxFileSize != idxFileSize_ || header.dictFileSize != dictFileSize_ || header.fingerprint != fingerprint ||
      header.checkpointCount == 0 || header.checkpointCount > MAX_CHECKPOINTS || header.stride == 0 ||
      header.offsetBits != (use64BitOffsets_ ? 64 : 32)) {
    return false;
  }
  const size_t bytes = static_cast<size_t>(header.checkpointCount) * sizeof(Checkpoint);
  if (cache.read(checkpoints_.data(), bytes) != static_cast<int>(bytes)) return false;
  for (size_t i = 0; i < header.checkpointCount; ++i) {
    if (checkpoints_[i].idxOffset >= idxFileSize_) return false;
    checkpoints_[i].entryText[CHECKPOINT_WORD_BYTES - 1] = '\0';
  }
  wordCount_ = header.wordCount;
  checkpointStride_ = header.stride;
  checkpointCount_ = header.checkpointCount;
  indexSorted_ = header.sorted != 0;
  return true;
}

void StarDictLookup::saveCheckpointCache(const std::string& cachePath, const uint32_t fingerprint) const {
  const std::string tempPath = cachePath + ".tmp";
  HalFile cache;
  if (!Storage.openFileForWrite("DICT", tempPath, cache)) return;
  CheckpointCacheHeader header{};
  memcpy(header.magic, CHECKPOINT_CACHE_MAGIC, sizeof(header.magic));
  header.version = CHECKPOINT_CACHE_VERSION;
  header.recordSize = sizeof(Checkpoint);
  header.idxFileSize = idxFileSize_;
  header.dictFileSize = dictFileSize_;
  header.fingerprint = fingerprint;
  header.wordCount = wordCount_;
  header.stride = checkpointStride_;
  header.checkpointCount = checkpointCount_;
  header.sorted = indexSorted_ ? 1 : 0;
  header.offsetBits = use64BitOffsets_ ? 64 : 32;
  const size_t recordsBytes = static_cast<size_t>(checkpointCount_) * sizeof(Checkpoint);
  const bool written = cache.write(&header, sizeof(header)) == sizeof(header) &&
                       cache.write(checkpoints_.data(), recordsBytes) == recordsBytes;
  cache.close();
  if (!written || !Storage.replaceFileFromTemp(cachePath.c_str(), tempPath.c_str())) Storage.remove(tempPath.c_str());
}

bool StarDictLookup::readIdxEntryAt(const uint32_t idxOffset, std::string& word, uint64_t& dictOffset,
                                    uint32_t& dictSize, uint32_t& nextOffset) {
  if (!idxFile_.seekSet(idxOffset) || !readCString(idxFile_, word)) return false;
  bool ok = false;
  dictOffset = use64BitOffsets_ ? readBe64(idxFile_, ok) : static_cast<uint64_t>(readBe32(idxFile_, ok));
  if (!ok) return false;
  dictSize = readBe32(idxFile_, ok);
  if (!ok) return false;
  nextOffset = static_cast<uint32_t>(idxFile_.position());
  return nextOffset > idxOffset && nextOffset <= idxFileSize_;
}

bool StarDictLookup::buildCheckpoints(const std::string& cachePath, const uint32_t fingerprint) {
  uint32_t offset = 0;
  uint32_t count = 0;
  checkpointCount_ = 0;
  indexSorted_ = true;
  // Estimate conservatively from the byte size, then compact again while
  // scanning if unusually short records would exceed the fixed RAM budget.
  const uint32_t estimatedEntries = std::max<uint32_t>(1, idxFileSize_ / 12);
  checkpointStride_ = std::max<uint32_t>(1, (estimatedEntries + MAX_CHECKPOINTS - 1) / MAX_CHECKPOINTS);
  std::string previousWord;
  uint8_t lastProgress = 255;
  while (offset < idxFileSize_) {
    std::string word;
    uint64_t dictOffset = 0;
    uint32_t dictSize = 0;
    uint32_t nextOffset = 0;
    if (!readIdxEntryAt(offset, word, dictOffset, dictSize, nextOffset)) return false;
    if (!previousWord.empty() && previousWord.compare(word) > 0) indexSorted_ = false;
    previousWord = word;

    if ((count % checkpointStride_) == 0) {
      if (checkpointCount_ == MAX_CHECKPOINTS) {
        const uint16_t compacted = static_cast<uint16_t>((checkpointCount_ + 1) / 2);
        for (uint16_t i = 1; i < compacted; ++i) checkpoints_[i] = checkpoints_[i * 2];
        checkpointCount_ = compacted;
        checkpointStride_ *= 2;
      }
      if ((count % checkpointStride_) == 0 && checkpointCount_ < MAX_CHECKPOINTS) {
        assignCheckpoint(checkpoints_[checkpointCount_++], offset, word);
      }
    }
    offset = nextOffset;
    ++count;
    if (progressCallback_ && idxFileSize_ > 0) {
      const uint8_t progress = static_cast<uint8_t>((static_cast<uint64_t>(offset) * 100u) / idxFileSize_);
      if (progress != lastProgress && (progress == 100 || progress % 5 == 0)) {
        lastProgress = progress;
        progressCallback_(progressContext_, offset, idxFileSize_);
      }
    }
  }
  if (wordCount_ == 0) wordCount_ = count;
  if (checkpointCount_ == 0) return false;
  saveCheckpointCache(cachePath, fingerprint);
  return true;
}

bool StarDictLookup::lookupViaCheckpoints(const std::string& candidate, uint64_t& dictOffset, uint32_t& dictSize) {
  if (checkpointCount_ == 0 || !indexSorted_) return false;
  size_t low = 0;
  size_t high = checkpointCount_;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (compareCheckpoint(checkpoints_[mid], candidate) <= 0)
      low = mid + 1;
    else
      high = mid;
  }
  // A query before the first checkpoint can still match the first bracket.
  const size_t bracket = low == 0 ? 0 : low - 1;
  uint32_t offset = checkpoints_[bracket].idxOffset;
  const uint32_t end = bracket + 1 < checkpointCount_ ? checkpoints_[bracket + 1].idxOffset : idxFileSize_;
  while (offset < end) {
    std::string word;
    uint32_t next = 0;
    if (!readIdxEntryAt(offset, word, dictOffset, dictSize, next)) break;
    const int cmp = word.compare(candidate);
    if (cmp == 0) return true;
    if (cmp > 0) break;
    offset = next;
  }
  return false;
}

bool StarDictLookup::lookupViaLinearScan(const std::string& candidateLower, uint64_t& dictOffset, uint32_t& dictSize) {
  uint32_t offset = 0;
  while (offset < idxFileSize_) {
    std::string word;
    uint32_t next = 0;
    if (!readIdxEntryAt(offset, word, dictOffset, dictSize, next)) break;
    if (lowerCopy(word) == candidateLower) return true;
    offset = next;
  }
  return false;
}

bool StarDictLookup::decodeDefinitionData(const std::string& raw, std::string& decoded) const {
  decoded.clear();
  size_t pos = 0;
  size_t sequencePos = 0;
  const bool hasSequence = !sameTypeSequence_.empty();
  while (pos < raw.size()) {
    char type = 0;
    if (hasSequence) {
      if (sequencePos >= sameTypeSequence_.size()) break;
      type = sameTypeSequence_[sequencePos++];
    } else {
      type = raw[pos++];
    }

    size_t fieldStart = pos;
    size_t fieldSize = 0;
    if (type >= 'a' && type <= 'z') {
      // With sametypesequence the final string field consumes the remaining
      // bytes and is not required to carry a trailing NUL.
      if (hasSequence && sequencePos == sameTypeSequence_.size()) {
        fieldSize = raw.size() - pos;
        pos = raw.size();
      } else {
        const size_t terminator = raw.find('\0', pos);
        if (terminator == std::string::npos) return false;
        fieldSize = terminator - pos;
        pos = terminator + 1;
      }
    } else if (type >= 'A' && type <= 'Z') {
      if (pos + 4 > raw.size()) return false;
      fieldSize = (static_cast<uint32_t>(static_cast<unsigned char>(raw[pos])) << 24) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(raw[pos + 1])) << 16) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(raw[pos + 2])) << 8) |
                  static_cast<uint32_t>(static_cast<unsigned char>(raw[pos + 3]));
      pos += 4;
      fieldStart = pos;
      if (fieldSize > raw.size() - pos) return false;
      pos += fieldSize;
    } else {
      return false;
    }

    // Lowercase StarDict fields are textual. Uppercase fields are binary
    // payloads (audio/images/resources) and must never be sent to the font or
    // HTML parsers. 'r' is a textual resource reference, useful to show when a
    // dictionary does not provide an inline definition.
    if (type >= 'a' && type <= 'z' && fieldSize > 0) {
      if (!decoded.empty()) decoded.push_back('\n');
      decoded.append(raw, fieldStart, fieldSize);
    }
  }
  return !decoded.empty();
}

bool StarDictLookup::lookup(const std::string& queryWord, std::string& outDefinition, bool* outTruncated) {
  outDefinition.clear();
  if (outTruncated) *outTruncated = false;
  if (!isOpen_) return false;
  const std::string cleaned = stripPunctuation(queryWord);
  if (cleaned.empty()) return false;

  const std::string lower = lowerCopy(cleaned);
  std::vector<std::string> candidates{cleaned, lower, titleCopy(cleaned)};
  for (const std::string& stem : stemCandidates(lower)) {
    candidates.push_back(stem);
    candidates.push_back(titleCopy(stem));
  }

  uint64_t dictOffset = 0;
  uint32_t dictSize = 0;
  bool found = false;
  for (const std::string& candidate : candidates) {
    if (lookupViaCheckpoints(candidate, dictOffset, dictSize)) {
      found = true;
      break;
    }
  }
  if (!found) {
    std::vector<std::string> lowerCandidates{lower};
    const auto stems = stemCandidates(lower);
    lowerCandidates.insert(lowerCandidates.end(), stems.begin(), stems.end());
    for (const std::string& candidate : lowerCandidates) {
      if (lookupViaLinearScan(candidate, dictOffset, dictSize)) {
        found = true;
        break;
      }
    }
  }
  if (!found || dictSize == 0 || dictOffset > dictFileSize_ || dictSize > dictFileSize_ - dictOffset ||
      !dictFile_.seek64(dictOffset)) {
    return false;
  }

  const uint32_t readSize = std::min(dictSize, MAX_DEFINITION_BYTES);
  std::string raw(readSize, '\0');
  if (dictFile_.read(raw.data(), readSize) != static_cast<int>(readSize)) {
    return false;
  }
  // A malformed type layout should not make an otherwise readable legacy
  // dictionary unusable. Fall back to the raw field, matching inx's behavior.
  if (!decodeDefinitionData(raw, outDefinition)) outDefinition = std::move(raw);
  if (outTruncated) *outTruncated = readSize < dictSize;
  return true;
}
