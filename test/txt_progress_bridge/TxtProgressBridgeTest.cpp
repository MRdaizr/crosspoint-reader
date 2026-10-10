#include <HalStorage.h>
#include <TxtBinary.h>
#include <TxtResumeOffset.h>
#include <gtest/gtest.h>

#include "util/TxtProgressBridge.h"

using Result = TxtProgressBridge::Result;
TEST(TxtResumeOffsetTest, ResumeRetainsParagraphBytesAndRewindsOnlyContinuationBytes) {
  const uint8_t text[] = {'A', 'B', '\n', 0xC3, 0xA9, 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80, '\n'};
  EXPECT_EQ(TxtResumeOffset::characterStart(text, sizeof(text), 1), 1u);
  EXPECT_EQ(TxtResumeOffset::characterStart(text, sizeof(text), 2), 2u);
  EXPECT_EQ(TxtResumeOffset::characterStart(text, sizeof(text), 4), 3u);
  EXPECT_EQ(TxtResumeOffset::characterStart(text, sizeof(text), 6), 5u);
  EXPECT_EQ(TxtResumeOffset::characterStart(text, sizeof(text), 7), 5u);
  EXPECT_EQ(TxtResumeOffset::characterStart(text, sizeof(text), 11), 8u);
  EXPECT_EQ(TxtResumeOffset::characterStart(text, sizeof(text), 12), 12u);
  EXPECT_EQ(TxtResumeOffset::characterStart(nullptr, 0, 0), 0u);
  EXPECT_EQ(TxtResumeOffset::characterStart(text, 3, 5), 5u);
}
class BridgeTest : public testing::Test {
 protected:
  std::string unified = "/cache/epub_" + std::to_string(std::hash<std::string>{}("/b.txt"));
  std::string legacy = "/cache/txt_" + std::to_string(std::hash<std::string>{}("/b.txt"));
  TxtSourceMap map{"/b.txt", unified};
  void SetUp() override {
    Storage.reset();
    Storage.files["/b.txt"].bytes =
        "\xEF\xBB\xBF"
        "A  中\r\nB😀C";
    ASSERT_TRUE(map.prepare());
  }
  void txpr(uint32_t source, uint32_t page = 4, uint32_t time = 10) {
    auto& value = Storage.files[legacy + "/progress.bin"];
    value.bytes.resize(12);
    auto* p = reinterpret_cast<uint8_t*>(value.bytes.data());
    TxtBinary::put32(p, 0x54585052);
    TxtBinary::put32(p + 4, page);
    TxtBinary::put32(p + 8, source);
    value.time = time;
  }
  void saveUnified(uint32_t visible, uint32_t time = 20) {
    auto& value = Storage.files[unified + "/progress.bin"];
    value.bytes.assign(10, 0);
    TxtBinary::put32(reinterpret_cast<uint8_t*>(value.bytes.data()) + 6, visible);
    value.time = time;
  }
  uint32_t uOffset() {
    return TxtBinary::get32(reinterpret_cast<const uint8_t*>(Storage.files[unified + "/progress.bin"].bytes.data()) +
                            6);
  }
  uint32_t lOffset() {
    return TxtBinary::get32(reinterpret_cast<const uint8_t*>(Storage.files[legacy + "/progress.bin"].bytes.data()) + 8);
  }
  void index(std::initializer_list<uint32_t> offsets, uint8_t version = 5) {
    auto& bytes = Storage.files[legacy + "/index.bin"].bytes;
    bytes.assign(30 + offsets.size() * 4, 0);
    auto* p = reinterpret_cast<uint8_t*>(bytes.data());
    TxtBinary::put32(p, 0x54585449);
    p[4] = version;
    TxtBinary::put32(p + 5, map.getSourceSize());
    TxtBinary::put32(p + 26, offsets.size());
    size_t offset = 30;
    for (auto value : offsets) {
      TxtBinary::put32(p + offset, value);
      offset += 4;
    }
  }
};
TEST_F(BridgeTest, TxprByteOffsetWinsOverLegacyPageHint) {
  txpr(6, 123456);
  TxtProgressBridge bridge("/b.txt", unified, map);
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  uint32_t expected;
  ASSERT_TRUE(map.sourceToVisible(6, expected));
  EXPECT_EQ(uOffset(), expected);
  EXPECT_EQ(Storage.files[unified + "/progress.bin"].bytes.substr(0, 6), std::string(6, 0));
  EXPECT_EQ(lOffset(), 6u);
  EXPECT_TRUE(Storage.exists((unified + "/progress_text_sync.bin").c_str()));
  EXPECT_EQ(bridge.migrateToUnified(), Result::Ready);
}
TEST_F(BridgeTest, OldPageOnlyRequiresCompleteMatchingV5Index) {
  Storage.files[legacy + "/progress.bin"].bytes = std::string("\1\0\3\0", 4);
  TxtProgressBridge bridge("/b.txt", unified, map);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
  EXPECT_FALSE(Storage.exists((unified + "/progress.bin").c_str()));
  index({0, 6, 11});
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  uint32_t expected;
  ASSERT_TRUE(map.sourceToVisible(6, expected));
  EXPECT_EQ(uOffset(), expected);
}
TEST_F(BridgeTest, InvalidIndexIsNeverGuessedOrRebuilt) {
  Storage.files[legacy + "/progress.bin"].bytes = std::string("\1\0\3\0", 4);
  const auto saved = Storage.files[legacy + "/progress.bin"].bytes;
  TxtProgressBridge bridge("/b.txt", unified, map);
  for (int variant = 0; variant < 6; ++variant) {
    index({0, 6, 11});
    auto& bytes = Storage.files[legacy + "/index.bin"].bytes;
    auto* p = reinterpret_cast<uint8_t*>(bytes.data());
    if (variant == 0) p[4] = 4;
    if (variant == 1) TxtBinary::put32(p + 5, map.getSourceSize() + 1);
    if (variant == 2) bytes.pop_back();
    if (variant == 3) TxtBinary::put32(p + 34, 7);  // inside CJK
    if (variant == 4) TxtBinary::put32(p + 38, 5);  // descending
    if (variant == 5) TxtBinary::put32(p + 38, map.getSourceSize() + 1);
    EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
    EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, saved);
    EXPECT_FALSE(Storage.exists((unified + "/progress.bin").c_str()));
  }
}
TEST_F(BridgeTest, InvalidTxprAndUnifiedPageOnlyRequireCompatibility) {
  TxtProgressBridge bridge("/b.txt", unified, map);
  txpr(7);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
  txpr(map.getSourceSize() + 1);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
  Storage.remove((legacy + "/progress.bin").c_str());
  Storage.files[unified + "/progress.bin"].bytes.assign(6, 0);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
}
TEST_F(BridgeTest, NewBookAndCancellationWriteNoProgress) {
  TxtProgressBridge bridge("/b.txt", unified, map);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NoProgress);
  txpr(6);
  TxtToHtml::Callbacks cb{nullptr, [](void*) { return true; }, nullptr};
  EXPECT_EQ(bridge.migrateToUnified(&cb), Result::Cancelled);
  EXPECT_FALSE(Storage.exists((unified + "/progress.bin").c_str()));
}
TEST_F(BridgeTest, BidirectionalMirrorAndNewLegacyAfterManualFallback) {
  txpr(6);
  TxtProgressBridge bridge("/b.txt", unified, map);
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  uint32_t visible;
  ASSERT_TRUE(map.sourceToVisible(11, visible));
  saveUnified(visible, 30);
  ASSERT_TRUE(bridge.mirrorUnifiedVisibleOffset(visible, 77));
  EXPECT_EQ(lOffset(), 11u);
  const auto* p = reinterpret_cast<const uint8_t*>(Storage.files[legacy + "/progress.bin"].bytes.data());
  EXPECT_EQ(TxtBinary::get32(p + 4), 77u);
  txpr(16, 900, 30);  // even equal FAT timestamps: changed legacy, unchanged unified wins.
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  ASSERT_TRUE(map.sourceToVisible(16, visible));
  EXPECT_EQ(uOffset(), visible);
  EXPECT_EQ(bridge.migrateToUnified(), Result::Ready);
}
TEST_F(BridgeTest, UpdatedUnifiedWithoutMirrorWinsOverUnchangedLegacy) {
  txpr(6);
  TxtProgressBridge bridge("/b.txt", unified, map);
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  uint32_t visible;
  ASSERT_TRUE(map.sourceToVisible(11, visible));
  saveUnified(visible, 1);
  ASSERT_EQ(bridge.migrateToUnified(), Result::Ready);
  EXPECT_EQ(lOffset(), 11u);
}
TEST_F(BridgeTest, ConflictingUnmarkedOrBothUpdatedRecordsRequireEvidence) {
  txpr(6, 2, 20);
  uint32_t visible;
  ASSERT_TRUE(map.sourceToVisible(11, visible));
  saveUnified(visible, 20);
  TxtProgressBridge bridge("/b.txt", unified, map);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
  Storage.files[legacy + "/progress.bin"].time = 21;
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  txpr(16, 9, 100);
  saveUnified(visible, 100);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
}
TEST_F(BridgeTest, AtomicWriteFailureRestoresExistingUnifiedAndLegacy) {
  txpr(6, 2, 100);
  saveUnified(1, 1);
  const auto old = Storage.files[unified + "/progress.bin"].bytes;
  Storage.failRename = unified + "/progress.bin";
  Storage.renameFailures = 1;
  Storage.exactRename = true;
  TxtProgressBridge bridge("/b.txt", unified, map);
  EXPECT_EQ(bridge.migrateToUnified(), Result::IoError);
  EXPECT_EQ(Storage.files[unified + "/progress.bin"].bytes, old);
  uint32_t visible;
  ASSERT_TRUE(map.sourceToVisible(11, visible));
  saveUnified(visible);
  const auto oldLegacy = Storage.files[legacy + "/progress.bin"].bytes;
  Storage.failWrite = legacy + "/progress.bin.tmp";
  EXPECT_FALSE(bridge.mirrorUnifiedVisibleOffset(visible, 0));
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, oldLegacy);
}
TEST_F(BridgeTest, ProgressPublicationPowerLossGapRecoversBackup) {
  txpr(6);
  TxtProgressBridge bridge("/b.txt", unified, map);
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  ASSERT_TRUE(Storage.rename((unified + "/progress.bin").c_str(), (unified + "/progress.bin.bridge.bak").c_str()));
  EXPECT_EQ(bridge.migrateToUnified(), Result::Ready);
  EXPECT_TRUE(Storage.exists((unified + "/progress.bin").c_str()));
}
TEST_F(BridgeTest, CloseFailureNeverPublishesProgressOrSyncMarker) {
  txpr(6, 2, 100);
  saveUnified(1, 1);
  const auto oldUnified = Storage.files[unified + "/progress.bin"].bytes;
  const auto oldLegacy = Storage.files[legacy + "/progress.bin"].bytes;
  TxtProgressBridge bridge("/b.txt", unified, map);
  Storage.failClose = unified + "/progress.bin.tmp";
  EXPECT_EQ(bridge.migrateToUnified(), Result::IoError);
  EXPECT_EQ(Storage.files[unified + "/progress.bin"].bytes, oldUnified);
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, oldLegacy);
  EXPECT_FALSE(Storage.exists((unified + "/progress_text_sync.bin").c_str()));
  Storage.failClose.clear();
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  const auto oldMarker = Storage.files[unified + "/progress_text_sync.bin"].bytes;
  uint32_t visible;
  ASSERT_TRUE(map.sourceToVisible(11, visible));
  saveUnified(visible);
  Storage.failClose = legacy + "/progress.bin.tmp";
  EXPECT_FALSE(bridge.mirrorUnifiedVisibleOffset(visible, 2));
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, oldLegacy);
  EXPECT_EQ(Storage.files[unified + "/progress_text_sync.bin"].bytes, oldMarker);
  Storage.failClose = "progress_text_sync.bin.tmp";
  EXPECT_FALSE(bridge.mirrorUnifiedVisibleOffset(visible, 2));
  EXPECT_EQ(Storage.files[unified + "/progress_text_sync.bin"].bytes, oldMarker);
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, oldLegacy);
}
TEST_F(BridgeTest, MarkerWriteOrRenameFailureRollsBackProgressBytes) {
  txpr(6, 2, 100);
  saveUnified(1, 1);
  const auto oldUnified = Storage.files[unified + "/progress.bin"].bytes;
  const auto oldLegacy = Storage.files[legacy + "/progress.bin"].bytes;
  TxtProgressBridge bridge("/b.txt", unified, map);
  Storage.failWrite = "progress_text_sync.bin.tmp";
  EXPECT_EQ(bridge.migrateToUnified(), Result::IoError);
  EXPECT_EQ(Storage.files[unified + "/progress.bin"].bytes, oldUnified);
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, oldLegacy);
  Storage.failWrite.clear();
  Storage.failRename = unified + "/progress_text_sync.bin";
  Storage.exactRename = true;
  Storage.renameFailures = 1;
  EXPECT_EQ(bridge.migrateToUnified(), Result::IoError);
  EXPECT_EQ(Storage.files[unified + "/progress.bin"].bytes, oldUnified);
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, oldLegacy);
}
TEST_F(BridgeTest, NormalReaderBackupRecoveryWinsOverStaleBridgeBackup) {
  txpr(6);
  TxtProgressBridge bridge("/b.txt", unified, map);
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  const auto original = Storage.files[legacy + "/progress.bin"].bytes;
  Storage.files[legacy + "/progress.bin.bridge.bak"].bytes = original;
  txpr(16, 30, 100);
  const auto latest = Storage.files[legacy + "/progress.bin"].bytes;
  ASSERT_TRUE(Storage.rename((legacy + "/progress.bin").c_str(), (legacy + "/progress.bin.bak").c_str()));
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, latest);
  uint32_t visible;
  ASSERT_TRUE(map.sourceToVisible(16, visible));
  EXPECT_EQ(uOffset(), visible);
  const auto unifiedBytes = Storage.files[unified + "/progress.bin"].bytes;
  ASSERT_TRUE(Storage.rename((unified + "/progress.bin").c_str(), (unified + "/progress.bin.bak").c_str()));
  ASSERT_EQ(bridge.migrateToUnified(), Result::Ready);
  EXPECT_EQ(Storage.files[unified + "/progress.bin"].bytes, unifiedBytes);
}
TEST_F(BridgeTest, SameSizeSourceUpdateCannotAuthenticatePageOnlyV5Index) {
  Storage.files[legacy + "/progress.bin"].bytes = std::string("\1\0\3\0", 4);
  index({0, 6, 11});
  const auto originalProgress = Storage.files[legacy + "/progress.bin"].bytes;
  Storage.files["/b.txt"].bytes[3] = 'Z';  // Preserve size and timestamp deliberately.
  TxtSourceMap updated("/b.txt", unified);
  ASSERT_TRUE(updated.prepare());
  ASSERT_TRUE(updated.wasSourceChanged());
  TxtProgressBridge bridge("/b.txt", unified, updated);
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, originalProgress);
  EXPECT_FALSE(Storage.exists((unified + "/progress.bin").c_str()));
  TxtSourceMap reopened("/b.txt", unified);
  ASSERT_TRUE(reopened.prepare(false));
  EXPECT_TRUE(reopened.wasSourceChanged());
  TxtProgressBridge next("/b.txt", unified, reopened);
  EXPECT_EQ(next.migrateToUnified(), Result::NeedsCompatibility);
  // An authoritative TXPR source position is still recoverable after fallback.
  txpr(11);
  EXPECT_EQ(next.migrateToUnified(), Result::Migrated);
}
TEST_F(BridgeTest, IndexOlderThanSourceOrWithoutTimestampsRequiresCompatibility) {
  Storage.files[legacy + "/progress.bin"].bytes = std::string("\1\0\3\0", 4);
  index({0, 6, 11});
  TxtProgressBridge bridge("/b.txt", unified, map);
  Storage.files["/b.txt"].time = 2;
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
  Storage.files["/b.txt"].time = 0;
  EXPECT_EQ(bridge.migrateToUnified(), Result::NeedsCompatibility);
}
TEST_F(BridgeTest, EofMirrorRequiresCompleteLegacyIndexAndNeverUsesUnifiedPageHint) {
  txpr(6);
  TxtProgressBridge bridge("/b.txt", unified, map);
  ASSERT_EQ(bridge.migrateToUnified(), Result::Migrated);
  const auto oldLegacy = Storage.files[legacy + "/progress.bin"].bytes;
  saveUnified(map.getVisibleCount() - 1);
  EXPECT_FALSE(bridge.mirrorUnifiedVisibleOffset(map.getVisibleCount() - 1, 9999));
  EXPECT_EQ(Storage.files[legacy + "/progress.bin"].bytes, oldLegacy);
  index({0, 6, 11});
  ASSERT_TRUE(bridge.mirrorUnifiedVisibleOffset(map.getVisibleCount() - 1, 9999));
  EXPECT_EQ(lOffset(), map.getSourceSize());
  const auto* p = reinterpret_cast<const uint8_t*>(Storage.files[legacy + "/progress.bin"].bytes.data());
  EXPECT_EQ(TxtBinary::get32(p + 4), 2u);
}
TEST_F(BridgeTest, CancellationInsideLargeIndexDoesNotWriteUnifiedProgress) {
  Storage.files[legacy + "/progress.bin"].bytes = std::string("\1\0\3\0", 4);
  index({0, 6, 11});
  bool stop = false;
  TxtToHtml::Callbacks cb{&stop, [](void* p) { return *static_cast<bool*>(p); },
                          [](void* p, uint32_t, uint32_t) { *static_cast<bool*>(p) = true; }};
  TxtProgressBridge bridge("/b.txt", unified, map);
  EXPECT_EQ(bridge.migrateToUnified(&cb), Result::Cancelled);
  EXPECT_FALSE(Storage.exists((unified + "/progress.bin").c_str()));
}
