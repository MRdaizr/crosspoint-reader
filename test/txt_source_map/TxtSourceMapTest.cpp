#include <HalStorage.h>
#include <TxtBinary.h>
#include <TxtSourceMap.h>
#include <gtest/gtest.h>

#include "../txt_to_html/TestHelpers.h"

class SourceMapTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
  void source(std::string text) { Storage.files["/b.txt"].bytes = std::move(text); }
};
TEST_F(SourceMapTest, ExactBodyCountAndBothDirections) {
  source(
      "\xEF\xBB\xBF"
      "  中\r\nA   B&<😀>\t\1  ");
  TxtSourceMap map("/b.txt", "/cache");
  ASSERT_TRUE(map.prepare());
  BodyOracle oracle;
  ASSERT_TRUE(oracle.parse(Storage.files[map.getHtmlPath()].bytes));
  EXPECT_EQ(map.getVisibleCount(), oracle.count);
  uint32_t visible, byte;
  EXPECT_TRUE(map.sourceToVisible(0, visible));
  EXPECT_EQ(visible, 1u);
  ASSERT_TRUE(map.sourceToVisible(5, visible));
  EXPECT_EQ(visible, 3u);  // CJK starts at byte 5.
  ASSERT_TRUE(map.sourceToVisible(6, visible));
  EXPECT_EQ(visible, 3u);  // Inside CJK.
  ASSERT_TRUE(map.visibleToSource(3, byte));
  EXPECT_EQ(byte, 5u);
  EXPECT_FALSE(map.isSourceBoundary(6));
  EXPECT_TRUE(map.isSourceBoundary(5));
  ASSERT_TRUE(map.sourceToVisible(map.getSourceSize(), visible));
  EXPECT_EQ(visible, oracle.count - 1);
  ASSERT_TRUE(map.visibleToSource(0, byte));
  EXPECT_EQ(byte, 0u);
  ASSERT_TRUE(map.visibleToSource(oracle.count, byte));
  EXPECT_EQ(byte, map.getSourceSize());
  for (uint32_t i = 1; i < oracle.count - 1; ++i) {
    ASSERT_TRUE(map.visibleToSource(i, byte));
    ASSERT_TRUE(map.isSourceBoundary(byte));
    ASSERT_TRUE(map.sourceToVisible(byte, visible));
    EXPECT_EQ(visible, i);
  }
  EXPECT_FALSE(map.sourceToVisible(map.getSourceSize() + 1, visible));
  EXPECT_FALSE(map.visibleToSource(oracle.count + 1, byte));
}
TEST_F(SourceMapTest, SparseUtf8CheckpointReplayAndDeferredSpacesAcrossBlocks) {
  source(std::string(4094, 'a') + "😀中" + "A" + std::string(9000, ' ') + "\r  B" + std::string(9000, ' ') + "\nZ");
  TxtSourceMap map("/b.txt", "/cache");
  ASSERT_TRUE(map.prepare());
  BodyOracle oracle;
  ASSERT_TRUE(oracle.parse(Storage.files[map.getHtmlPath()].bytes));
  EXPECT_EQ(map.getVisibleCount(), oracle.count);
  uint32_t byte, visible;
  for (uint32_t i = 1; i < oracle.count - 1; i += 53) {
    ASSERT_TRUE(map.visibleToSource(i, byte));
    EXPECT_TRUE(map.isSourceBoundary(byte));
    ASSERT_TRUE(map.sourceToVisible(byte, visible));
    EXPECT_EQ(visible, i);
  }
  // The map occupies fixed records, not one entry per source byte/character.
  size_t mapSize = 0;
  for (const auto& [path, record] : Storage.files)
    if (path.ends_with(".map")) mapSize = record.bytes.size();
  EXPECT_LT(mapSize, 256u);
  EXPECT_LE(Storage.maxReadRequested, 1024u);
  ASSERT_TRUE(map.sourceToVisible(4095, visible));
  EXPECT_EQ(visible, 4095u);
  ASSERT_TRUE(map.visibleToSource(4095, byte));
  EXPECT_EQ(byte, 4094u);
}
TEST_F(SourceMapTest, DeferredSpaceRunWithIgnoredCrUsesSourceAnchors) {
  source("A \r \r  B \r \nC  ");
  TxtSourceMap map("/b.txt", "/cache");
  ASSERT_TRUE(map.prepare());
  uint32_t byte, visible;
  const uint32_t anchors[] = {0, 1, 3, 5, 6, 7, 12};
  for (uint32_t i = 0; i < 7; ++i) {
    ASSERT_TRUE(map.visibleToSource(i + 1, byte));
    EXPECT_EQ(byte, anchors[i]);
    ASSERT_TRUE(map.sourceToVisible(byte, visible));
    EXPECT_EQ(visible, i + 1);
  }
  ASSERT_TRUE(map.sourceToVisible(2, visible));
  EXPECT_EQ(visible, 3u);
  ASSERT_TRUE(map.sourceToVisible(8, visible));
  EXPECT_EQ(visible, 7u);
}
TEST_F(SourceMapTest, SameSizeContentReplacementIsDetectedAndRecordsPreserved) {
  source("ABC\n");
  TxtSourceMap first("/b.txt", "/cache");
  ASSERT_TRUE(first.prepare());
  const auto hash = first.getSourceHash();
  const auto html = first.getHtmlPath();
  Storage.files["/cache/progress.bin"].bytes = "progress";
  Storage.files["/cache/bookmarks.bin"].bytes = "bookmark";
  Storage.files["/cache/progress_text_sync.bin"].bytes = "precedence";
  Storage.mkdir("/cache/sections");
  Storage.files["/cache/sections/0.bin"].bytes = "stale";
  source("XYZ\n");
  TxtSourceMap second("/b.txt", "/cache");
  ASSERT_TRUE(second.prepare());
  EXPECT_NE(hash, second.getSourceHash());
  EXPECT_NE(html, second.getHtmlPath());
  EXPECT_EQ(Storage.files["/cache/progress.bin"].bytes, "progress");
  EXPECT_EQ(Storage.files["/cache/bookmarks.bin"].bytes, "bookmark");
  EXPECT_EQ(Storage.files["/cache/progress_text_sync.bin"].bytes, "precedence");
  EXPECT_FALSE(Storage.exists("/cache/sections/0.bin"));
  EXPECT_TRUE(Storage.files[second.getHtmlPath()].bytes.find("XYZ") != std::string::npos);
}
TEST_F(SourceMapTest, CancellationAndWriteFailureNeverPublishPartialGeneration) {
  source("old");
  TxtSourceMap old("/b.txt", "/cache");
  ASSERT_TRUE(old.prepare());
  const auto manifest = Storage.files["/cache/text.manifest"].bytes;
  source(std::string(5000, 'z'));
  unsigned ticks = 0;
  TxtToHtml::Callbacks cb{&ticks, [](void* p) { return ++*static_cast<unsigned*>(p) > 12; }, nullptr};
  TxtSourceMap cancelled("/b.txt", "/cache");
  EXPECT_FALSE(cancelled.prepare(true, &cb));
  EXPECT_EQ(Storage.files["/cache/text.manifest"].bytes, manifest);
  Storage.failWrite = ".map.tmp";
  TxtSourceMap failed("/b.txt", "/cache");
  EXPECT_FALSE(failed.prepare());
  EXPECT_EQ(Storage.files["/cache/text.manifest"].bytes, manifest);
}
TEST_F(SourceMapTest, FailedManifestRenameRetainsOldGenerationAndPowerLossRecovery) {
  source("old");
  TxtSourceMap old("/b.txt", "/cache");
  ASSERT_TRUE(old.prepare());
  const auto manifest = Storage.files["/cache/text.manifest"].bytes;
  source("new");
  Storage.failRename = "text.manifest";
  Storage.renameFailures = 1;
  TxtSourceMap failed("/b.txt", "/cache");
  EXPECT_FALSE(failed.prepare());
  EXPECT_EQ(Storage.files["/cache/text.manifest"].bytes, manifest);
  source("old");
  ASSERT_TRUE(Storage.rename("/cache/text.manifest", "/cache/text.manifest.bak"));
  TxtSourceMap recovered("/b.txt", "/cache");
  EXPECT_TRUE(recovered.prepare(false));
  EXPECT_EQ(recovered.getHtmlPath(), old.getHtmlPath());
}
TEST_F(SourceMapTest, SourceMutationDuringConversionCannotPublishMixedGeneration) {
  source(std::string(5000, 'a'));
  bool mutated = false;
  TxtToHtml::Callbacks cb{&mutated, nullptr, [](void* p, uint32_t bytes, uint32_t) {
                            auto& changed = *static_cast<bool*>(p);
                            if (bytes > 0 && !changed) {
                              Storage.files["/b.txt"].bytes[0] = 'b';
                              changed = true;
                            }
                          }};
  TxtSourceMap map("/b.txt", "/cache");
  EXPECT_FALSE(map.prepare(true, &cb));
  EXPECT_FALSE(Storage.exists("/cache/text.manifest"));
}
TEST_F(SourceMapTest, CorruptHtmlOrMapCannotBeLoadedAsAConsistentCache) {
  source("hello");
  TxtSourceMap old("/b.txt", "/cache");
  ASSERT_TRUE(old.prepare());
  Storage.files[old.getHtmlPath()].bytes[0] = '!';
  TxtSourceMap invalid("/b.txt", "/cache");
  EXPECT_FALSE(invalid.prepare(false));
  EXPECT_TRUE(invalid.prepare());
  for (auto& [path, record] : Storage.files)
    if (path.ends_with(".map")) record.bytes[12] ^= 1;
  TxtSourceMap mapInvalid("/b.txt", "/cache");
  EXPECT_FALSE(mapInvalid.prepare(false));
}
TEST_F(SourceMapTest, CloseFailureOfHtmlMapOrManifestCannotPublish) {
  source("old");
  TxtSourceMap old("/b.txt", "/cache");
  ASSERT_TRUE(old.prepare());
  const auto manifest = Storage.files["/cache/text.manifest"].bytes;
  source("new");
  for (const char* failure : {".html.tmp", ".map.tmp", "text.manifest.tmp"}) {
    Storage.failClose = failure;
    TxtSourceMap failed("/b.txt", "/cache");
    EXPECT_FALSE(failed.prepare());
    EXPECT_EQ(Storage.files["/cache/text.manifest"].bytes, manifest);
    EXPECT_EQ(Storage.files[old.getHtmlPath()].bytes.find("old") != std::string::npos, true);
  }
}
TEST_F(SourceMapTest, OneByteSdReadsKeepBomAndMultibyteInputSafe) {
  source(
      "\xEF\xBB\xBF"
      "中😀é\r\nA  B");
  Storage.maxRead = 1;
  TxtSourceMap map("/b.txt", "/cache");
  ASSERT_TRUE(map.prepare());
  Storage.maxRead = SIZE_MAX;
  BodyOracle oracle;
  ASSERT_TRUE(oracle.parse(Storage.files[map.getHtmlPath()].bytes));
  EXPECT_EQ(map.getVisibleCount(), oracle.count);
  uint32_t visible, offset;
  ASSERT_TRUE(map.sourceToVisible(5, visible));
  EXPECT_EQ(visible, 1u);
  ASSERT_TRUE(map.visibleToSource(2, offset));
  EXPECT_EQ(offset, 6u);
}
TEST_F(SourceMapTest, LargeInputDoesNotReadWholeBookForPositionLookups) {
  source(std::string(256 * 1024, 'a') + "😀\r\nZ");
  TxtSourceMap map("/b.txt", "/cache");
  ASSERT_TRUE(map.prepare());
  Storage.readBytes = 0;
  uint32_t visible, offset;
  ASSERT_TRUE(map.sourceToVisible(128 * 1024 + 37, visible));
  ASSERT_TRUE(map.visibleToSource(visible, offset));
  EXPECT_EQ(offset, 128 * 1024 + 37);
  EXPECT_LT(Storage.readBytes, 12 * 1024u);
  EXPECT_LE(Storage.maxReadRequested, 1024u);
}
TEST_F(SourceMapTest, LongSpaceReplayPulsesAndCancelsWithoutRtosOnHost) {
  source("A" + std::string(64 * 1024, ' ') + "B");
  TxtSourceMap map("/b.txt", "/cache");
  ASSERT_TRUE(map.prepare());
  unsigned pulses = 0;
  TxtToHtml::Callbacks cb{&pulses, [](void* p) { return *static_cast<unsigned*>(p) >= 3; },
                          [](void* p, uint32_t, uint32_t) { ++*static_cast<unsigned*>(p); }};
  uint32_t result = 1234;
  EXPECT_FALSE(map.visibleToSource(30000, result, &cb));
  EXPECT_EQ(result, 1234u);
  EXPECT_GE(pulses, 3u);
}
