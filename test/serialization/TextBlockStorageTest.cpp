#include <Epub/blocks/TextBlock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace {
class TextBlockStorageTest : public ::testing::Test {
 protected:
  std::string path;
  void SetUp() override {
    path = (std::filesystem::temp_directory_path() /
            ("crosspoint-text-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
               .string();
  }
  void TearDown() override { std::filesystem::remove(path); }
  std::unique_ptr<TextBlock> read(const std::vector<uint8_t>& bytes) {
    HalFile file;
    if (!file.open(path.c_str(), "wb") || file.write(bytes.data(), bytes.size()) != bytes.size()) return nullptr;
    file.close();
    if (!file.open(path.c_str(), "rb")) return nullptr;
    return TextBlock::deserialize(file);
  }
  std::vector<uint8_t> save(const TextBlock& block) {
    HalFile file;
    EXPECT_TRUE(file.open(path.c_str(), "wb"));
    EXPECT_TRUE(block.serialize(file));
    file.close();
    EXPECT_TRUE(file.open(path.c_str(), "rb"));
    std::vector<uint8_t> bytes(file.size());
    EXPECT_EQ(file.read(bytes.data(), bytes.size()), bytes.size());
    return bytes;
  }
};
}  // namespace

TEST_F(TextBlockStorageTest, PlainCjkPageDoesNotAllocateRubyTable) {
  const std::vector<std::string> words(512, "中");
  const std::vector<int16_t> positions(512, 0);
  const std::vector<EpdFontFamily::Style> styles(512, EpdFontFamily::REGULAR);
  TextBlock original(words, positions, styles, {}, {});
  auto decoded = read(save(original));
  ASSERT_TRUE(decoded);
  EXPECT_FALSE(decoded->hasRuby());
  EXPECT_EQ(decoded->getRubyTexts().capacity(), 0u);
  EXPECT_EQ(decoded->wordCount(), 512u);
  EXPECT_STREQ(decoded->wordText(511), "中");
  TextBlock emptyRuby(words, positions, styles, {}, {}, {}, std::vector<std::string>(512));
  EXPECT_EQ(emptyRuby.getRubyTexts().capacity(), 0u);
}

TEST_F(TextBlockStorageTest, SparseRubyAndFocusRetainIndexedAnnotationsAndOffsets) {
  TextBlock original({"plain", "word", "日本"}, {0, 40, 80},
                     std::vector<EpdFontFamily::Style>(3, EpdFontFamily::REGULAR), {1, 1, 0}, {8, 8, 0}, {},
                     {"", "", "にほん"});
  auto decoded = read(save(original));
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(decoded->hasRuby());
  ASSERT_EQ(decoded->getRubyTexts().size(), 3u);
  EXPECT_EQ(decoded->getRubyTexts()[2], "にほん");
  EXPECT_TRUE(decoded->getRubyTexts()[0].empty());
  EXPECT_EQ(decoded->getRubyShift(20), 10);
  EXPECT_EQ(decoded->focusBoundary(1), 1);
  EXPECT_EQ(decoded->focusSuffixX(1), 8);
}

TEST_F(TextBlockStorageTest, RejectsEveryTruncatedPrefixAndInvalidStyleBoolean) {
  TextBlock original({"word"}, {0}, {EpdFontFamily::REGULAR}, {}, {});
  auto bytes = save(original);
  for (size_t size = 0; size < bytes.size(); ++size)
    EXPECT_FALSE(read(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size))) << size;
  // 5 header bytes + 5 arena metadata bytes + 5 text bytes + 4 empty-ruby bytes.
  // The style starts with one-byte alignment followed by textAlignDefined.
  bytes[20] = 2;
  EXPECT_FALSE(read(bytes));
}
