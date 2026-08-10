#include <HalStorage.h>
#include <StarDictLookup.h>
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {
void writeBe32(std::ostream& out, uint32_t value) {
  const std::array<unsigned char, 4> bytes{static_cast<unsigned char>(value >> 24),
                                           static_cast<unsigned char>(value >> 16),
                                           static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void writeBe64(std::ostream& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) out.put(static_cast<char>((value >> shift) & 0xff));
}

class StarDictLookupTest : public testing::Test {
 protected:
  std::filesystem::path root;
  void SetUp() override {
    const auto* testInfo = testing::UnitTest::GetInstance()->current_test_info();
    const char* testName = testInfo == nullptr ? "unknown" : testInfo->name();
    root = std::filesystem::temp_directory_path() / (std::string("inkpoint-stardict-test-") + testName);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "dictionaries" / "test");
    Storage.setRoot(root);
  }
  void TearDown() override { std::filesystem::remove_all(root); }

  void createDictionary(const std::vector<std::pair<std::string, std::string>>& entries, bool offsets64 = false,
                        const char* typeSequence = "h") {
    const auto folder = root / "dictionaries" / "test";
    std::ofstream dict(folder / "test.dict", std::ios::binary);
    std::ofstream idx(folder / "test.idx", std::ios::binary);
    uint64_t offset = 0;
    for (const auto& [word, definition] : entries) {
      dict.write(definition.data(), definition.size());
      idx.write(word.c_str(), word.size() + 1);
      if (offsets64)
        writeBe64(idx, offset);
      else
        writeBe32(idx, static_cast<uint32_t>(offset));
      writeBe32(idx, static_cast<uint32_t>(definition.size()));
      offset += definition.size();
    }
    dict.close();
    idx.close();
    std::ofstream ifo(folder / "test.ifo");
    ifo << "StarDict's dict ifo file\nversion=3.0.0\nbookname=Test\nwordcount=" << entries.size()
        << "\nidxfilesize=1\nsametypesequence=" << typeSequence << "\n";
    if (offsets64) ifo << "idxoffsetbits=64\n";
  }
};
}  // namespace

TEST_F(StarDictLookupTest, ExactCaseAndEnglishStemLookups) {
  createDictionary({{"Hello", "<b>Greeting</b>"}, {"book", "A written work"}, {"run", "Move quickly"}});
  StarDictLookup dictionary;
  ASSERT_TRUE(dictionary.open("/dictionaries/test"));
  std::string definition;
  EXPECT_TRUE(dictionary.lookup("hello,", definition));
  EXPECT_EQ(definition, "<b>Greeting</b>");
  EXPECT_TRUE(dictionary.lookup("books", definition));
  EXPECT_EQ(definition, "A written work");
  EXPECT_TRUE(dictionary.lookup("running", definition));
  EXPECT_EQ(definition, "Move quickly");
}

TEST_F(StarDictLookupTest, FallsBackForNonStandardIndexOrder) {
  createDictionary({{"zeta", "last"}, {"apple", "first"}});
  StarDictLookup dictionary;
  ASSERT_TRUE(dictionary.open("/dictionaries/test"));
  std::string definition;
  EXPECT_TRUE(dictionary.lookup("APPLE", definition));
  EXPECT_EQ(definition, "first");
}

TEST_F(StarDictLookupTest, Supports64BitIndexOffsets) {
  createDictionary({{"alpha", "definition"}}, true);
  StarDictLookup dictionary;
  ASSERT_TRUE(dictionary.open("/dictionaries/test"));
  std::string definition;
  EXPECT_TRUE(dictionary.lookup("alpha", definition));
  EXPECT_EQ(definition, "definition");
}

TEST_F(StarDictLookupTest, DecodesMultipleTextAndSkipsBinaryFields) {
  std::string article = "phonetic";
  article.push_back('\0');
  article.append("<b>definition</b>");
  article.push_back('\0');
  article.append("\0\0\0\x03", 4);
  article.append("bin", 3);
  createDictionary({{"word", article}}, false, "thP");
  StarDictLookup dictionary;
  ASSERT_TRUE(dictionary.open("/dictionaries/test"));
  std::string definition;
  EXPECT_TRUE(dictionary.lookup("word", definition));
  EXPECT_EQ(definition, "phonetic\n<b>definition</b>");
}

TEST_F(StarDictLookupTest, CapsOversizedDefinitions) {
  createDictionary({{"large", std::string(StarDictLookup::MAX_DEFINITION_BYTES + 100, 'x')}});
  StarDictLookup dictionary;
  ASSERT_TRUE(dictionary.open("/dictionaries/test"));
  std::string definition;
  bool truncated = false;
  EXPECT_TRUE(dictionary.lookup("large", definition, &truncated));
  EXPECT_TRUE(truncated);
  EXPECT_EQ(definition.size(), StarDictLookup::MAX_DEFINITION_BYTES);
}

TEST_F(StarDictLookupTest, RejectsIncompleteDictionarySet) {
  std::ofstream(root / "dictionaries" / "test" / "only.ifo") << "version=3.0.0\n";
  StarDictLookup dictionary;
  EXPECT_FALSE(dictionary.open("/dictionaries/test"));
}

TEST_F(StarDictLookupTest, CloseIsSafeBeforeOpenAfterFailureAndRepeatedly) {
  StarDictLookup dictionary;
  dictionary.close();
  dictionary.close();

  std::ofstream(root / "dictionaries" / "test" / "only.ifo") << "version=3.0.0\n";
  EXPECT_FALSE(dictionary.open("/dictionaries/test"));
  dictionary.close();
  dictionary.close();

  createDictionary({{"word", "definition"}});
  ASSERT_TRUE(dictionary.open("/dictionaries/test"));
  dictionary.close();
  dictionary.close();
  EXPECT_FALSE(dictionary.isOpen());
}

TEST_F(StarDictLookupTest, PersistsAndValidatesCompactCheckpointCache) {
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(900);
  for (int i = 0; i < 900; ++i) {
    char word[24];
    snprintf(word, sizeof(word), "word-%04d", i);
    entries.emplace_back(word, "definition-" + std::to_string(i));
  }
  createDictionary(entries);

  {
    StarDictLookup dictionary;
    ASSERT_TRUE(dictionary.open("/dictionaries/test"));
    std::string definition;
    ASSERT_TRUE(dictionary.lookup("word-0899", definition));
    EXPECT_EQ(definition, "definition-899");
  }

  const auto cache = root / "dictionaries" / "test" / ".inkpointx-dictionary.idx";
  ASSERT_TRUE(std::filesystem::exists(cache));

  // A second open exercises the sidecar rather than rebuilding it.
  StarDictLookup reopened;
  ASSERT_TRUE(reopened.open("/dictionaries/test"));
  std::string definition;
  ASSERT_TRUE(reopened.lookup("word-0001", definition));
  EXPECT_EQ(definition, "definition-1");

  // Replacing the index with different bytes invalidates the sampled
  // fingerprint even when a stale sidecar remains beside it.
  createDictionary({{"alpha", "new definition"}});
  StarDictLookup replaced;
  ASSERT_TRUE(replaced.open("/dictionaries/test"));
  ASSERT_TRUE(replaced.lookup("alpha", definition));
  EXPECT_EQ(definition, "new definition");
}

TEST_F(StarDictLookupTest, RejectsDefinitionOutsideDictFile) {
  createDictionary({{"word", "definition"}});
  auto idxPath = root / "dictionaries" / "test" / "test.idx";
  std::fstream idx(idxPath, std::ios::binary | std::ios::in | std::ios::out);
  idx.seekp(5);  // word + NUL
  writeBe32(idx, 0x7ffffff0u);
  idx.close();

  StarDictLookup dictionary;
  ASSERT_TRUE(dictionary.open("/dictionaries/test"));
  std::string definition;
  EXPECT_FALSE(dictionary.lookup("word", definition));
}
