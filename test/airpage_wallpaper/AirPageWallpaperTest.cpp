#include <AirPageWallpaper.h>
#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <gtest/gtest.h>

// These tests exercise the production publication/rollback code. Image codec
// validity is stubbed; actual BMP integrity remains covered by AirPageImageStoreTest.
bool airpage::AirPageImageStore::inspectImage(const char* path, ImageInfo& info) {
  if (!Storage.exists(path) || !wallpaperfake::files[path].starts_with("BM:")) return false;
  info.format = ImageFormat::Bmp;
  info.width = info.height = 1;
  return true;
}
class AirPageWallpaperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wallpaperfake::reset();
    SETTINGS = {};
    JpegToBmpConverter::decodeResult = true;
    wallpaperfake::files["/sleep.bmp"] = "BM:old";
    wallpaperfake::files["/book/cover.bmp"] = "BM:new";
    wallpaperfake::files["/book/cover.jpg"] = "jpeg";
    wallpaperfake::files["/book/cover.png"] = "PNG:new";
  }
  void oldImageSurvives() {
    EXPECT_EQ(wallpaperfake::files["/sleep.bmp"], "BM:old");
    EXPECT_EQ(SETTINGS.sleepScreen, 0);
  }
};
TEST_F(AirPageWallpaperTest, PublishesBmpAndSavesModeWithoutChangingSource) {
  ASSERT_TRUE(airpage::AirPageWallpaper::installFile("/book/cover.bmp", airpage::ImageFormat::Bmp));
  EXPECT_EQ(wallpaperfake::files["/sleep.bmp"], "BM:new");
  EXPECT_EQ(wallpaperfake::files["/book/cover.bmp"], "BM:new");
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::CUSTOM);
  EXPECT_FALSE(Storage.exists("/sleep.bmp.part"));
  EXPECT_FALSE(Storage.exists("/sleep.bmp.bak"));
}
TEST_F(AirPageWallpaperTest, JpegConversionUsesCropAndOrdinaryCustomMode) {
  SETTINGS.sleepScreen = CrossPointSettings::TRANSPARENT_CUSTOM;
  ASSERT_TRUE(airpage::AirPageWallpaper::installFile("/book/cover.jpg", airpage::ImageFormat::Jpeg, true));
  EXPECT_TRUE(JpegToBmpConverter::lastCrop);
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::CUSTOM);
  EXPECT_EQ(wallpaperfake::files["/sleep.bmp"], "BM:converted");
  EXPECT_FALSE(Storage.exists("/sleep-overlay.bmp"));
}
TEST_F(AirPageWallpaperTest, DecodeFailureRetainsOldImageAndMode) {
  JpegToBmpConverter::decodeResult = false;
  EXPECT_FALSE(airpage::AirPageWallpaper::installFile("/book/cover.jpg", airpage::ImageFormat::Jpeg));
  oldImageSurvives();
}
TEST_F(AirPageWallpaperTest, ShortWriteRetainsOldImageAndMode) {
  wallpaperfake::failWrite = true;
  EXPECT_FALSE(airpage::AirPageWallpaper::installFile("/book/cover.bmp", airpage::ImageFormat::Bmp));
  oldImageSurvives();
}
TEST_F(AirPageWallpaperTest, ReadFailureRetainsOldImageAndMode) {
  wallpaperfake::failRead = true;
  EXPECT_FALSE(airpage::AirPageWallpaper::installFile("/book/cover.bmp", airpage::ImageFormat::Bmp));
  oldImageSurvives();
}
TEST_F(AirPageWallpaperTest, CloseFailureRetainsOldImageAndMode) {
  wallpaperfake::failClose = true;
  EXPECT_FALSE(airpage::AirPageWallpaper::installFile("/book/cover.bmp", airpage::ImageFormat::Bmp));
  oldImageSurvives();
}
TEST_F(AirPageWallpaperTest, InvalidCopiedImageIsNotPublished) {
  wallpaperfake::files["/book/cover.bmp"] = "truncated";
  EXPECT_FALSE(airpage::AirPageWallpaper::installFile("/book/cover.bmp", airpage::ImageFormat::Bmp));
  oldImageSurvives();
}
TEST_F(AirPageWallpaperTest, BothRenameFailurePointsRetainOldImage) {
  for (int failure = 0; failure < 2; ++failure) {
    wallpaperfake::failRenameAfter = failure;
    EXPECT_FALSE(airpage::AirPageWallpaper::installFile("/book/cover.bmp", airpage::ImageFormat::Bmp));
    oldImageSurvives();
  }
}
TEST_F(AirPageWallpaperTest, SettingsFailureRollsBackImageAndMemoryMode) {
  SETTINGS.saveResult = false;
  EXPECT_FALSE(airpage::AirPageWallpaper::installFile("/book/cover.bmp", airpage::ImageFormat::Bmp));
  oldImageSurvives();
}
TEST_F(AirPageWallpaperTest, InstallingSelectedSleepImageDoesNotTruncateIt) {
  ASSERT_TRUE(airpage::AirPageWallpaper::installFile("/sleep.bmp", airpage::ImageFormat::Bmp));
  EXPECT_EQ(wallpaperfake::files["/sleep.bmp"], "BM:old");
}
TEST_F(AirPageWallpaperTest, InterruptedUncommittedPublicationRestoresBackup) {
  wallpaperfake::files["/sleep.bmp.bak"] = "BM:backup";
  wallpaperfake::files["/sleep.bmp"] = "BM:uncommitted";
  wallpaperfake::files["/sleep.bmp.part"] = "partial";
  airpage::AirPageWallpaper::recoverInterruptedTransaction();
  EXPECT_EQ(wallpaperfake::files["/sleep.bmp"], "BM:backup");
  EXPECT_FALSE(Storage.exists("/sleep.bmp.part"));
}
TEST_F(AirPageWallpaperTest, CommittedValidPublicationKeepsNewImage) {
  SETTINGS.sleepScreen = CrossPointSettings::CUSTOM;
  wallpaperfake::files["/sleep.bmp.bak"] = "BM:backup";
  airpage::AirPageWallpaper::recoverInterruptedTransaction();
  EXPECT_EQ(wallpaperfake::files["/sleep.bmp"], "BM:old");
  EXPECT_FALSE(Storage.exists("/sleep.bmp.bak"));
}
TEST_F(AirPageWallpaperTest, InvalidCommittedImageStillRestoresBackup) {
  SETTINGS.sleepScreen = CrossPointSettings::CUSTOM;
  wallpaperfake::files["/sleep.bmp.bak"] = "BM:backup";
  wallpaperfake::files["/sleep.bmp"] = "truncated";
  airpage::AirPageWallpaper::recoverInterruptedTransaction();
  EXPECT_EQ(wallpaperfake::files["/sleep.bmp"], "BM:backup");
}
TEST_F(AirPageWallpaperTest, OverlayFormatReplacementAndRollbackPreserveOtherFormat) {
  SETTINGS.sleepScreen = CrossPointSettings::TRANSPARENT_CUSTOM;
  wallpaperfake::files["/sleep-overlay.bmp"] = "BM:old-overlay";
  SETTINGS.saveResult = false;
  EXPECT_FALSE(airpage::AirPageWallpaper::installOverlay("/book/cover.png", "/sleep-overlay.png"));
  EXPECT_EQ(wallpaperfake::files["/sleep-overlay.bmp"], "BM:old-overlay");
  EXPECT_FALSE(Storage.exists("/sleep-overlay.png"));
  SETTINGS.saveResult = true;
  EXPECT_TRUE(airpage::AirPageWallpaper::installOverlay("/book/cover.png", "/sleep-overlay.png"));
  EXPECT_FALSE(Storage.exists("/sleep-overlay.bmp"));
  EXPECT_EQ(wallpaperfake::files["/sleep-overlay.png"], "PNG:new");
  EXPECT_EQ(SETTINGS.sleepScreen, CrossPointSettings::TRANSPARENT_CUSTOM);
}
