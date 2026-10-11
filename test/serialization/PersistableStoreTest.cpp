#include <HalStorage.h>
#include <PersistableStore.h>
#include <gtest/gtest.h>

namespace {
constexpr char PATH[] = "/.crosspoint/settings.json";
constexpr char TMP[] = "/.crosspoint/settings.json.tmp";
constexpr char BAK[] = "/.crosspoint/settings.json.bak";
class PersistableStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.files[PATH].bytes = "{\"value\":1}";
  }
  bool save() {
    JsonDocument doc;
    doc["value"] = 2;
    return PersistableStoreBase::writeDocToFile(PATH, doc);
  }
};
}  // namespace

TEST_F(PersistableStoreTest, PublishesCompleteJsonAndReadsThroughBufferedAdapter) {
  ASSERT_TRUE(save());
  EXPECT_FALSE(Storage.exists(TMP));
  EXPECT_FALSE(Storage.exists(BAK));
  JsonDocument doc;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(PATH, doc));
  EXPECT_EQ(doc["value"].as<int>(), 2);
  EXPECT_LE(Storage.maxReadRequested, 128u);
}

TEST_F(PersistableStoreTest, FailedWriteAndClosePreservePreviousSnapshot) {
  Storage.failWrite = TMP;
  EXPECT_FALSE(save());
  EXPECT_EQ(Storage.files[PATH].bytes, "{\"value\":1}");
  Storage.failWrite.clear();
  Storage.failClose = TMP;
  EXPECT_FALSE(save());
  EXPECT_EQ(Storage.files[PATH].bytes, "{\"value\":1}");
  EXPECT_FALSE(Storage.exists(TMP));
}

TEST_F(PersistableStoreTest, OverflowedDocumentCannotReplacePreviousSnapshot) {
  class NoMemoryAllocator : public ArduinoJson::Allocator {
   public:
    void* allocate(size_t) override { return nullptr; }
    void* reallocate(void*, size_t) override { return nullptr; }
    void deallocate(void*) override {}
  } allocator;
  JsonDocument doc(&allocator);
  doc["value"] = "cannot allocate";
  ASSERT_TRUE(doc.overflowed());
  EXPECT_FALSE(PersistableStoreBase::writeDocToFile(PATH, doc));
  EXPECT_EQ(Storage.files[PATH].bytes, "{\"value\":1}");
}

TEST_F(PersistableStoreTest, FailedPublicationRestoresOldFile) {
  Storage.failRename = PATH;
  Storage.exactRename = true;
  Storage.renameFailures = 1;
  EXPECT_FALSE(save());
  EXPECT_EQ(Storage.files[PATH].bytes, "{\"value\":1}");
}

TEST_F(PersistableStoreTest, InterruptedPublicationRecoversBackupAndIgnoresTemp) {
  Storage.files[BAK] = Storage.files[PATH];
  Storage.files.erase(PATH);
  Storage.files[TMP].bytes = "{\"value\":";
  EXPECT_TRUE(PersistableStoreBase::hasDocFile(PATH));
  JsonDocument doc;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(PATH, doc));
  EXPECT_EQ(doc["value"].as<int>(), 1);
  EXPECT_TRUE(Storage.exists(PATH));
}

TEST_F(PersistableStoreTest, FailedRollbackRetainsRecoverableBackup) {
  Storage.failRename = PATH;
  Storage.exactRename = true;
  Storage.renameFailures = 2;
  EXPECT_FALSE(save());
  EXPECT_FALSE(Storage.exists(PATH));
  EXPECT_EQ(Storage.files[BAK].bytes, "{\"value\":1}");
  JsonDocument doc;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(PATH, doc));
  EXPECT_EQ(doc["value"].as<int>(), 1);
}

TEST_F(PersistableStoreTest, PrefersValidCanonicalButCanReadUndamagedBackup) {
  Storage.files[BAK].bytes = "{\"value\":9}";
  JsonDocument doc;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(PATH, doc));
  EXPECT_EQ(doc["value"].as<int>(), 1);
  Storage.files[PATH].bytes = "{\"value\":";
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(PATH, doc));
  EXPECT_EQ(doc["value"].as<int>(), 9);
}

TEST_F(PersistableStoreTest, RejectsOversizedShortReadAndTrailingGarbage) {
  JsonDocument doc;
  Storage.files[PATH].bytes = std::string(64 * 1024 + 1, ' ');
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(PATH, doc));
  Storage.files[PATH].bytes = "{\"value\":1} garbage";
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(PATH, doc));
  Storage.files[PATH].bytes = "{\"value\":1}";
  Storage.maxRead = 4;
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(PATH, doc));
}
