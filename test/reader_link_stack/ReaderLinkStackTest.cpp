#include <gtest/gtest.h>

#include "src/util/ReaderLinkStack.h"

using readerlinks::Position;

TEST(ReaderLinkStack, KeepsNewestOriginsWhenFull) {
  Position origins[readerlinks::MAX_DEPTH]{};
  int depth = 0;
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(readerlinks::push(origins, depth, {i, i * 10}));
  ASSERT_EQ(depth, 3);
  EXPECT_EQ(origins[0].spineIndex, 2);
  EXPECT_EQ(origins[1].spineIndex, 3);
  EXPECT_EQ(origins[--depth].pageNumber, 40);
  EXPECT_EQ(origins[--depth].pageNumber, 30);
  EXPECT_EQ(origins[--depth].pageNumber, 20);
}

TEST(ReaderLinkStack, PersistedBytesAreLittleEndianAndRestoreReturnOrder) {
  const Position origins[] = {{0x1234, 0x3456}, {2, 0}, {3, 65534}};
  uint8_t data[readerlinks::MAX_BYTES]{};
  ASSERT_EQ(readerlinks::encode(origins, 3, data), 13u);
  EXPECT_EQ(data[0], 3);
  EXPECT_EQ(data[1], 0x34);
  EXPECT_EQ(data[2], 0x12);
  EXPECT_EQ(data[3], 0x56);
  EXPECT_EQ(data[4], 0x34);
  Position restored[readerlinks::MAX_DEPTH]{};
  ASSERT_EQ(readerlinks::decode(data, sizeof(data), 5000, restored), 3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(restored[i].spineIndex, origins[i].spineIndex);
    EXPECT_EQ(restored[i].pageNumber, origins[i].pageNumber);
  }
}

TEST(ReaderLinkStack, RejectsEveryTruncatedRecordWithoutPublishingPositions) {
  const Position origins[] = {{1, 10}, {2, 20}, {3, 30}};
  uint8_t data[readerlinks::MAX_BYTES]{};
  ASSERT_EQ(readerlinks::encode(origins, 3, data), sizeof(data));
  for (size_t size = 0; size < sizeof(data); ++size) {
    Position restored[readerlinks::MAX_DEPTH] = {{99, 88}};
    EXPECT_EQ(readerlinks::decode(data, size, 4, restored), 0) << size;
    EXPECT_EQ(restored[0].spineIndex, 99);
    EXPECT_EQ(restored[0].pageNumber, 88);
  }
}

TEST(ReaderLinkStack, RejectsOversizedDepthAndTrailingData) {
  const uint8_t oversizedDepth[] = {4, 0, 0, 0, 0};
  const uint8_t trailing[] = {1, 0, 0, 0, 0, 0};
  const uint8_t empty[] = {0};
  Position restored[readerlinks::MAX_DEPTH]{};
  EXPECT_EQ(readerlinks::decode(oversizedDepth, sizeof(oversizedDepth), 4, restored), 0);
  EXPECT_EQ(readerlinks::decode(trailing, sizeof(trailing), 4, restored), 0);
  EXPECT_EQ(readerlinks::decode(empty, sizeof(empty), 4, restored), 0);
}

TEST(ReaderLinkStack, RejectsSpineOutsideBookAndLastPageSentinel) {
  const uint8_t badSpine[] = {1, 4, 0, 0, 0};
  const uint8_t sentinel[] = {1, 1, 0, 0xFF, 0xFF};
  Position restored[readerlinks::MAX_DEPTH] = {{99, 88}};
  EXPECT_EQ(readerlinks::decode(badSpine, sizeof(badSpine), 4, restored), 0);
  EXPECT_EQ(readerlinks::decode(sentinel, sizeof(sentinel), 4, restored), 0);
  EXPECT_EQ(restored[0].spineIndex, 99);
}

TEST(ReaderLinkStack, InvalidOriginDoesNotDropOldestEntry) {
  Position origins[readerlinks::MAX_DEPTH] = {{1, 10}, {2, 20}, {3, 30}};
  int depth = readerlinks::MAX_DEPTH;
  EXPECT_FALSE(readerlinks::push(origins, depth, {4, UINT16_MAX}));
  EXPECT_FALSE(readerlinks::push(origins, depth, {-1, 10}));
  EXPECT_EQ(depth, readerlinks::MAX_DEPTH);
  EXPECT_EQ(origins[0].spineIndex, 1);
}

TEST(ReaderLinkStack, RefusesUnrepresentableOrEmptyCache) {
  uint8_t data[readerlinks::MAX_BYTES]{};
  Position origins[] = {{65536, 1}};
  EXPECT_EQ(readerlinks::encode(origins, 1, data), 0u);
  origins[0] = {1, -1};
  EXPECT_EQ(readerlinks::encode(origins, 1, data), 0u);
  EXPECT_EQ(readerlinks::encode(origins, 0, data), 0u);
}
