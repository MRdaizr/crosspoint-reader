#include <FsHelpers.h>
#include <gtest/gtest.h>

TEST(ImageExtensions, MixedCaseImageFormatsShareViewerRouting) {
  for (const std::string_view path : {"/Book.JPG", "photo.JpEg", "cover.PnG", "image.BMP"}) {
    EXPECT_TRUE(FsHelpers::hasImageExtension(path)) << path;
  }
  for (const std::string_view path : {"x.jpeg.part", "photo.jpg.bak", "book.epub", "image.gif", ".jpgx", ""}) {
    EXPECT_FALSE(FsHelpers::hasImageExtension(path)) << path;
  }
}

TEST(BookExtensions, ReflowableBooksDoNotIncludeImagesOrFixedLayoutBooks) {
  for (const std::string_view path : {"book.EPUB", "text.TxT", "notes.MD"}) {
    EXPECT_TRUE(FsHelpers::hasReflowableBookExtension(path));
  }
  for (const std::string_view path : {"photo.jpeg", "book.xtc", "notes.md.tmp", "x"}) {
    EXPECT_FALSE(FsHelpers::hasReflowableBookExtension(path));
  }
}

TEST(FileNames, SingleComponentCannotEscapeItsDirectory) {
  for (const std::string_view name : {"", ".", "..", "../book", "books/file", "books\\file"}) {
    EXPECT_FALSE(FsHelpers::isSafePathComponent(name));
  }
  EXPECT_TRUE(FsHelpers::isSafePathComponent(std::string_view("volume..2.epub")));
}
