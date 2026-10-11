#include <BookMetadataCache.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <sstream>
#include <vector>

namespace {
constexpr char PATH[] = "/cache/book.bin";
template <typename T>
void replace(std::string& bytes, const size_t offset, const T value) {
  memcpy(bytes.data() + offset, &value, sizeof(value));
}
std::string fixture() {
  std::ostringstream metadata;
  for (const char* value : {"Title", "Author", "zh", "", ""}) serialization::writeString(metadata, value);
  std::ostringstream spine;
  for (int i = 0; i < 3; ++i) {
    serialization::writeString(spine, "ch" + std::to_string(i) + ".xhtml");
    serialization::writePod(spine, uint32_t((i + 1) * 100));
    serialization::writePod(spine, int16_t(i == 0 ? 0 : -1));
  }
  std::ostringstream toc;
  for (const char* value : {"Chapter", "ch0.xhtml", "start"}) serialization::writeString(toc, value);
  serialization::writePod(toc, uint8_t(0));
  serialization::writePod(toc, int16_t(0));
  const uint32_t lutOffset = 9 + metadata.str().size();
  const uint32_t dataStart = lutOffset + 4 * sizeof(uint32_t);
  std::ostringstream out;
  serialization::writePod(out, uint8_t(10));
  serialization::writePod(out, lutOffset);
  serialization::writePod(out, uint16_t(3));
  serialization::writePod(out, uint16_t(1));
  out << metadata.str();
  for (unsigned i = 0; i < 3; ++i) serialization::writePod(out, dataStart + i * 19);
  serialization::writePod(out, dataStart + uint32_t(spine.str().size()));
  out << spine.str() << toc.str();
  return out.str();
}
class BookMetadataCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.files[PATH].bytes = fixture();
  }
};
}  // namespace

TEST_F(BookMetadataCacheTest, LoadsCompatibleCacheAndPositions) {
  BookMetadataCache cache("/cache");
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.coreMetadata.title, "Title");
  EXPECT_EQ(cache.getSpineCount(), 3);
  EXPECT_EQ(cache.getCumulativeSize(2), 300u);
  EXPECT_EQ(cache.getSpineEntry(1).href, "ch1.xhtml");
  EXPECT_EQ(cache.getTocEntry(0).anchor, "start");
}

TEST_F(BookMetadataCacheTest, MissingCacheDoesNotCloseAnUnopenedHandle) {
  Storage.files.erase(PATH);
  BookMetadataCache cache("/cache");
  EXPECT_FALSE(cache.load());
  EXPECT_FALSE(cache.isLoaded());
  EXPECT_EQ(cache.getSpineCount(), 0);
  EXPECT_EQ(Storage.invalidCloseCalls, 0u);
}

TEST_F(BookMetadataCacheTest, RejectsEveryTruncatedPrefix) {
  const auto valid = fixture();
  for (size_t size = 0; size < valid.size(); ++size) {
    Storage.files[PATH].bytes = valid.substr(0, size);
    BookMetadataCache cache("/cache");
    EXPECT_FALSE(cache.load()) << size;
    EXPECT_FALSE(cache.isLoaded());
    EXPECT_EQ(cache.getSpineCount(), 0);
  }
}

TEST_F(BookMetadataCacheTest, RejectsOversizedStringsAndInvalidLutWithoutPublishingState) {
  for (int corruption = 0; corruption < 4; ++corruption) {
    auto bytes = fixture();
    uint32_t lut = 0;
    memcpy(&lut, bytes.data() + 1, sizeof(lut));
    if (corruption == 0) replace(bytes, 9, UINT32_MAX);
    if (corruption == 1) replace(bytes, 1, UINT32_MAX);
    if (corruption == 2) replace(bytes, lut, uint32_t(0));
    if (corruption == 3) replace(bytes, lut + 3 * sizeof(uint32_t), UINT32_MAX);
    Storage.files[PATH].bytes = bytes;
    BookMetadataCache cache("/cache");
    EXPECT_FALSE(cache.load()) << corruption;
    EXPECT_TRUE(cache.coreMetadata.title.empty());
  }
}

TEST_F(BookMetadataCacheTest, ShortReadAndLaterLutDamageAreReported) {
  Storage.maxRead = 2;
  BookMetadataCache cache("/cache");
  EXPECT_FALSE(cache.load());
  Storage.maxRead = SIZE_MAX;
  ASSERT_TRUE(cache.load());
  auto& bytes = Storage.files[PATH].bytes;
  uint32_t lut = 0;
  memcpy(&lut, bytes.data() + 1, sizeof(lut));
  replace(bytes, lut, UINT32_MAX);
  EXPECT_TRUE(cache.getSpineEntry(0).href.empty());
  EXPECT_FALSE(cache.isLoaded());
}
