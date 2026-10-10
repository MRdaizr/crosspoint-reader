#include <ProgressFile.h>
#include <gtest/gtest.h>

class ProgressFileTest : public ::testing::Test {
 protected:
  const std::string cache = "/cache";
  const std::string final = cache + "/progress.bin";
  const uint8_t next[4] = {4, 3, 2, 1};
  void SetUp() override {
    Storage.reset();
    Storage.files[final] = {"old", 1};
  }
};

TEST_F(ProgressFileTest, PublishesClosedCompleteFile) {
  ASSERT_TRUE(ProgressFile::writeAtomic(cache, next, sizeof(next)));
  EXPECT_EQ(Storage.files[final].bytes, std::string(reinterpret_cast<const char*>(next), sizeof(next)));
  EXPECT_FALSE(Storage.exists((final + ".bak").c_str()));
}
TEST_F(ProgressFileTest, ShortWritePreservesOldPosition) {
  Storage.failWrite = ".tmp";
  EXPECT_FALSE(ProgressFile::writeAtomic(cache, next, sizeof(next)));
  EXPECT_EQ(Storage.files[final].bytes, "old");
}
TEST_F(ProgressFileTest, FailedClosePreservesOldPosition) {
  Storage.failClose = ".tmp";
  EXPECT_FALSE(ProgressFile::writeAtomic(cache, next, sizeof(next)));
  EXPECT_EQ(Storage.files[final].bytes, "old");
}
TEST_F(ProgressFileTest, FailedPublicationRollsBack) {
  Storage.failRename = final;
  Storage.exactRename = true;
  Storage.renameFailures = 1;
  EXPECT_FALSE(ProgressFile::writeAtomic(cache, next, sizeof(next)));
  EXPECT_EQ(Storage.files[final].bytes, "old");
}
TEST_F(ProgressFileTest, RecoversPowerCutBetweenRenames) {
  ASSERT_TRUE(Storage.rename(final.c_str(), (final + ".bak").c_str()));
  Storage.files[final + ".tmp"] = {"torn", 2};
  ASSERT_TRUE(ProgressFile::recover(cache));
  EXPECT_EQ(Storage.files[final].bytes, "old");
}
TEST_F(ProgressFileTest, PublishedPositionWinsOverBackup) {
  Storage.files[final + ".bak"] = {"older", 0};
  ASSERT_TRUE(ProgressFile::recover(cache));
  EXPECT_EQ(Storage.files[final].bytes, "old");
}
TEST_F(ProgressFileTest, CannotPublishWhileRecoveryFails) {
  ASSERT_TRUE(Storage.rename(final.c_str(), (final + ".bak").c_str()));
  Storage.failRename = final;
  Storage.exactRename = true;
  Storage.renameFailures = 2;
  EXPECT_FALSE(ProgressFile::writeAtomic(cache, next, sizeof(next)));
  EXPECT_EQ(Storage.files[final + ".bak"].bytes, "old");
  EXPECT_FALSE(Storage.exists((final + ".tmp").c_str()));
}
TEST_F(ProgressFileTest, RejectsEmptyData) {
  EXPECT_FALSE(ProgressFile::writeAtomic(cache, nullptr, 4));
  EXPECT_FALSE(ProgressFile::writeAtomic(cache, next, 0));
  EXPECT_EQ(Storage.files[final].bytes, "old");
}
