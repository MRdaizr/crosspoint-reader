#include <Epub/VisibleOffsetLut.h>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

struct LutFile {
  std::vector<uint32_t> starts;
  size_t position = 0, reads = 0;
  size_t failAtRead = 0;
  int read(uint8_t* data, size_t bytes) {
    ++reads;
    if (failAtRead == reads) return static_cast<int>(bytes) - 1;
    const size_t count = std::min(bytes / 4, starts.size() - position);
    std::memcpy(data, starts.data() + position, count * 4);
    position += count;
    return static_cast<int>(count * 4);
  }
};
TEST(VisibleOffsetLutTest, UsesThreeReadsFor65Pages) {
  LutFile file;
  for (unsigned i = 0; i < 65; ++i) file.starts.push_back(i * 10);
  EXPECT_EQ(64, pageForVisibleOffset(file, 65, 640, false, false));
  EXPECT_EQ(3u, file.reads);
}
TEST(VisibleOffsetLutTest, LastPageHandlesOffsetsBeyondCompleteLut) {
  LutFile file{{0, 10, 20}};
  EXPECT_EQ(2, pageForVisibleOffset(file, 3, 1000, false, false));
}
TEST(VisibleOffsetLutTest, PartialCacheMustNotGuessBeyondItsWatermark) {
  LutFile file{{0, 10, 20}};
  EXPECT_FALSE(pageForVisibleOffset(file, 3, 21, false, true));
}
TEST(VisibleOffsetLutTest, PartialCacheAcceptsExactLastStart) {
  LutFile file{{0, 10, 20}};
  EXPECT_EQ(2, pageForVisibleOffset(file, 3, 20, false, true));
}
TEST(VisibleOffsetLutTest, DuplicateStartsAcrossBatchBoundaryRespectPreference) {
  for (const bool first : {false, true}) {
    LutFile file;
    for (unsigned i = 0; i < 31; ++i) file.starts.push_back(i);
    file.starts.insert(file.starts.end(), {31, 31, 31, 40});
    EXPECT_EQ(first ? 31 : 33, pageForVisibleOffset(file, 35, 31, first, false));
  }
}
TEST(VisibleOffsetLutTest, ShortReadDoesNotReturnAValidPage) {
  LutFile file;
  file.starts.resize(64);
  file.failAtRead = 2;
  EXPECT_FALSE(pageForVisibleOffset(file, 64, 5, false, false));
}
TEST(VisibleOffsetLutTest, EmptyLutDoesNotRead) {
  LutFile file;
  EXPECT_FALSE(pageForVisibleOffset(file, 0, 0, true, true));
  EXPECT_EQ(0u, file.reads);
}
TEST(VisibleOffsetLutTest, UsesOnlyDiskCountDuringIncrementalRebuild) {
  LutFile file{{0, 10, 20, 1000, 2000}};
  EXPECT_EQ(2, pageForVisibleOffset(file, 3, 20, false, true));
  EXPECT_EQ(3u, file.position);
}
