#include <Epub/WordStore.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <string>

namespace {
unsigned failChunkAllocations = 0;
}
void* operator new[](size_t size) {
  if (void* p = std::malloc(size ? size : 1)) return p;
  std::abort();
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (size == 2048 && failChunkAllocations) {
    --failChunkAllocations;
    return nullptr;
  }
  return std::malloc(size ? size : 1);
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

TEST(WordStoreTest, WordsShareNulTerminatedUtf8Chunk) {
  WordStore store;
  WordStore::StoredWord a, b;
  ASSERT_TRUE(store.append("中文", 6, a));
  ASSERT_TRUE(store.append("日本語", 9, b));
  EXPECT_EQ(a.chunk, b.chunk);
  EXPECT_EQ("中文", store.view(a));
  EXPECT_STREQ("日本語", store.cstr(b));
  EXPECT_EQ(1u, store.chunkCount());
}

TEST(WordStoreTest, NonTailChunkRetiresOnlyAfterLastWord) {
  WordStore store;
  WordStore::StoredWord a, b, c;
  const std::string large(1900, 'x');
  ASSERT_TRUE(store.append(large.data(), large.size(), a));
  ASSERT_TRUE(store.append("live", 4, b));
  ASSERT_TRUE(store.append(large.data(), large.size(), c));
  store.release(a);
  ASSERT_NE(nullptr, store.chunkData(a.chunk));
  EXPECT_EQ("live", store.view(b));
  store.release(b);
  EXPECT_EQ(nullptr, store.chunkData(a.chunk));
  EXPECT_EQ(large, store.view(c));
}

TEST(WordStoreTest, SuffixSharesOriginalReleaseObligation) {
  WordStore store;
  WordStore::StoredWord original, other;
  ASSERT_TRUE(store.append("abcdefgh", 8, original));
  auto suffix = WordStore::suffix(original, 3);
  const std::string large(2048, 'x');
  ASSERT_TRUE(store.append(large.data(), large.size(), other));
  EXPECT_EQ("defgh", store.view(suffix));
  store.release(suffix);
  EXPECT_EQ(nullptr, store.chunkData(original.chunk));
}

TEST(WordStoreTest, DrainedTailReusesItsSpaceAcrossSoftFlushes) {
  WordStore store;
  for (unsigned i = 0; i < 3000; ++i) {
    WordStore::StoredWord word;
    ASSERT_TRUE(store.append("paragraph", 9, word));
    EXPECT_EQ("paragraph", store.view(word));
    store.release(word);
  }
  EXPECT_EQ(1u, store.chunkCount());
  EXPECT_EQ(10u, store.chunkUsed(0));
}

TEST(WordStoreTest, OversizedUtf8IsRejectedWithoutChangingOutput) {
  WordStore store;
  WordStore::StoredWord out{7, 8, 9};
  const std::string large(65535, 'x');
  EXPECT_FALSE(store.append(large.data(), large.size(), out));
  EXPECT_EQ(7u, out.chunk);
  EXPECT_EQ(8u, out.off);
  EXPECT_EQ(9u, out.len);
  EXPECT_EQ(0u, store.chunkCount());
}

TEST(WordStoreTest, OomLeavesOutputAndExistingWordsIntact) {
  WordStore store;
  WordStore::StoredWord live, out{7, 8, 9};
  const std::string large(1900, 'x');
  ASSERT_TRUE(store.append(large.data(), large.size(), live));
  failChunkAllocations = 1;
  EXPECT_FALSE(store.append(large.data(), large.size(), out));
  EXPECT_EQ(0u, failChunkAllocations);
  EXPECT_EQ(7u, out.chunk);
  EXPECT_EQ(8u, out.off);
  EXPECT_EQ(9u, out.len);
  EXPECT_EQ(large, store.view(live));
}
