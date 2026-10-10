#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "BookCacheUtils.h"
#include "Epub.h"
#include "Txt.h"
#include "Xtc.h"

namespace {
class BookCacheUtilsTest : public ::testing::Test {
 protected:
  const std::vector<std::string> globalPaths{
      "/.crosspoint/reading_stats.json",   "/.crosspoint/reading_stats.json.tmp",
      "/.crosspoint/bookmarks/global.bin", "/.crosspoint/bookmarks/global.bin.tmp",
      "/.crosspoint/recents.json",         "/.crosspoint/library.idx"};
  std::map<std::string, fake::Node> globals;
  void SetUp() override {
    fake::reset();
    cachetest::clearCalls.clear();
    for (const auto& path : globalPaths) {
      fake::add(path, "user data sentinel: " + path);
      globals.emplace(path, *fake::files[path]);
    }
    fake::mutations.clear();
  }
  void expectGlobalsUnchanged() {
    for (const auto& [path, before] : globals) {
      SCOPED_TRACE(path);
      ASSERT_TRUE(fake::files.contains(path));
      EXPECT_EQ(*fake::files[path], before);
    }
    for (const auto& operation : fake::mutations) {
      EXPECT_NE(operation.from, "/.crosspoint");
      for (const auto& path : globalPaths) {
        EXPECT_NE(operation.from, path);
        EXPECT_NE(operation.to, path);
      }
      EXPECT_FALSE(operation.from.starts_with("/.crosspoint/bookmarks"));
      EXPECT_FALSE(operation.to.starts_with("/.crosspoint/bookmarks"));
    }
  }
  static std::vector<std::string> seedCache(const std::string& cache) {
    const std::vector<std::string> retained{"progress.bin",
                                            "progress.bin.tmp",
                                            "progress.bak",
                                            "progress_42.bin",
                                            "progress_checkpoint",
                                            "legacy_progress.bin",
                                            "legacy_progress.bin.tmp",
                                            "legacy_progress_recovery",
                                            "txt_progress.bin",
                                            "txt_progress.bin.tmp",
                                            "txt_progress_42",
                                            "bookmarks.bin",
                                            "progress_saves/nested/resume.bin"};
    for (const auto& entry : retained) fake::add(cache + "/" + entry, "keep " + entry);
    for (const auto* entry : {"book.bin", "cover.bmp", "thumb.bmp", "text.manifest", "metadata.json", "unknown.tmp",
                              "sections/1.bin", "sections/nested/2.bin"})
      fake::add(cache + "/" + entry, "regenerate");
    return retained;
  }
  static void expectRetained(const std::string& cache, const std::vector<std::string>& retained) {
    ASSERT_TRUE(Storage.exists(cache.c_str()));
    for (const auto& entry : retained) {
      SCOPED_TRACE(entry);
      const auto found = fake::files.find(cache + "/" + entry);
      ASSERT_NE(found, fake::files.end());
      const std::string expected = "keep " + entry;
      EXPECT_EQ(found->second->bytes, std::vector<uint8_t>(expected.begin(), expected.end()));
    }
    for (const auto* entry :
         {"book.bin", "cover.bmp", "thumb.bmp", "text.manifest", "metadata.json", "unknown.tmp", "sections"})
      EXPECT_FALSE(Storage.exists((cache + "/" + entry).c_str())) << entry;
  }
};

TEST_F(BookCacheUtilsTest, CachePathDispatchUsesCorrectNamespaceAndCaseInsensitiveExtensions) {
  for (const auto* extension : {"epub", "EPUB", "txt", "TXT", "md", "MD", "xtc", "XTC"}) {
    SCOPED_TRACE(extension);
    const std::string book = std::string("/Books/novel.") + extension;
    const std::string path = getBookCachePath(book);
    const bool text = extension[0] == 't' || extension[0] == 'T' || extension[0] == 'm' || extension[0] == 'M';
    const char* kind = text ? "txt" : extension[0] == 'x' || extension[0] == 'X' ? "xtc" : "epub";
    EXPECT_EQ(path, cachetest::cachePath(kind, book, "/.crosspoint"));
  }
  EXPECT_TRUE(getBookCachePath("/Books/book.epub.bak").empty());
  EXPECT_TRUE(getBookCachePath("/Books/image.jpg").empty());
  EXPECT_TRUE(getBookCachePath("").empty());
  EXPECT_TRUE(fake::mutations.empty());
}

TEST_F(BookCacheUtilsTest, DirectoryRecognitionHasNoBroadHiddenDirectoryMatch) {
  for (const auto* name : {"epub_123", "txt_123", "xtc_123"}) EXPECT_TRUE(isBookCacheDirectoryName(name));
  for (const auto* name : {"", "epub", "txt", "xtc", "epub123", "other_epub_123", "bookmarks", "reading_stats.json"})
    EXPECT_FALSE(isBookCacheDirectoryName(name));
  EXPECT_FALSE(isBookCacheDirectoryName(nullptr));
}

TEST_F(BookCacheUtilsTest, InvalidateEpubAndXtcOnlyRemovesRegenerableEntries) {
  for (const auto* book : {"/Books/novel.epub", "/Books/novel.xtc"}) {
    SCOPED_TRACE(book);
    const std::string cache = getBookCachePath(book);
    const auto retained = seedCache(cache);
    fake::mutations.clear();
    ASSERT_TRUE(invalidateBookCache(book));
    expectRetained(cache, retained);
    expectGlobalsUnchanged();
    EXPECT_TRUE(cachetest::clearCalls.empty());  // selective invalidator, not clearCache
    fake::mutations.clear();
    ASSERT_TRUE(invalidateBookCache(book));
    EXPECT_TRUE(fake::mutations.empty());  // retained-only directories are idempotent
  }
}

TEST_F(BookCacheUtilsTest, InvalidateTxtAndMarkdownPreservesResumeFamiliesInBothNamespaces) {
  for (const auto* book : {"/Books/novel.txt", "/Books/note.md", "/Books/uppercase.MD"}) {
    SCOPED_TRACE(book);
    const std::string legacy = Txt(book, "/.crosspoint").getCachePath();
    const std::string unified = Epub(book, "/.crosspoint").getCachePath();
    ASSERT_NE(legacy, unified);
    const auto keptLegacy = seedCache(legacy), keptUnified = seedCache(unified);
    fake::mutations.clear();
    ASSERT_TRUE(invalidateBookCache(book));
    expectRetained(legacy, keptLegacy);
    expectRetained(unified, keptUnified);
    expectGlobalsUnchanged();
    EXPECT_TRUE(cachetest::clearCalls.empty());
  }
}

TEST_F(BookCacheUtilsTest, MissingPrimaryTxtCacheDoesNotSkipExistingUnifiedCache) {
  const std::string book = "/Books/only-unified.txt";
  const std::string unified = Epub(book, "/.crosspoint").getCachePath();
  const auto retained = seedCache(unified);
  fake::mutations.clear();
  ASSERT_TRUE(invalidateBookCache(book));
  expectRetained(unified, retained);
  expectGlobalsUnchanged();
}

TEST_F(BookCacheUtilsTest, MissingCachesAndUnsupportedFilesAreReadOnlyNoOps) {
  const std::string irrelevant = Epub("/Books/photo.jpg", "/.crosspoint").getCachePath();
  seedCache(irrelevant);
  const auto before = fake::snapshot();
  fake::mutations.clear();
  for (const auto* path : {"/Books/missing.epub", "/Books/missing.txt", "/Books/missing.md", "/Books/missing.xtc",
                           "/Books/photo.jpg", ""}) {
    EXPECT_TRUE(invalidateBookCache(path));
    clearBookCache(path);
  }
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
  expectGlobalsUnchanged();
}

TEST_F(BookCacheUtilsTest, InvalidCacheHandleAndAllocationFailureDoNotDeleteUserData) {
  const std::string book = "/Books/failed.epub", cache = getBookCachePath(book);
  seedCache(cache);
  const auto before = fake::snapshot();
  fake::mutations.clear();
  fake::failOpenPath = cache;
  EXPECT_FALSE(invalidateBookCache(book));
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
  fake::failOpenPath.clear();
  fake::failAlloc = 0;
  EXPECT_FALSE(invalidateBookCache(book));
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
  EXPECT_TRUE(fake::failureTriggered);
  expectGlobalsUnchanged();
}

TEST_F(BookCacheUtilsTest, FileAtCacheDirectoryPathIsRejectedWithoutMutation) {
  const std::string book = "/Books/bad.epub", cache = getBookCachePath(book);
  fake::add(cache, "not a directory");
  const auto before = fake::snapshot();
  fake::mutations.clear();
  EXPECT_FALSE(invalidateBookCache(book));
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
}

TEST_F(BookCacheUtilsTest, FailedLegacyRemovalStillInvalidatesUnifiedTxtNamespace) {
  const std::string book = "/Books/partial.txt";
  const std::string legacy = Txt(book, "/.crosspoint").getCachePath();
  const std::string unified = Epub(book, "/.crosspoint").getCachePath();
  const auto retainedLegacy = seedCache(legacy), retainedUnified = seedCache(unified);
  fake::failRemovePath = legacy + "/book.bin";
  fake::mutations.clear();
  EXPECT_FALSE(invalidateBookCache(book));
  EXPECT_TRUE(Storage.exists(fake::failRemovePath.c_str()));
  for (const auto& entry : retainedLegacy) EXPECT_TRUE(Storage.exists((legacy + "/" + entry).c_str()));
  expectRetained(unified, retainedUnified);
  expectGlobalsUnchanged();
}

TEST_F(BookCacheUtilsTest, ClearTxtAndMarkdownDispatchesBothNamespaces) {
  for (const auto* book : {"/Books/legacy.txt", "/Books/unified.md"}) {
    SCOPED_TRACE(book);
    const std::string legacy = Txt(book, "/.crosspoint").getCachePath();
    const std::string unified = Epub(book, "/.crosspoint").getCachePath();
    seedCache(legacy);
    seedCache(unified);
    cachetest::clearCalls.clear();
    fake::mutations.clear();
    clearBookCache(book);
    ASSERT_EQ(cachetest::clearCalls.size(), 2u);
    EXPECT_EQ(cachetest::clearCalls[0].kind, "txt");
    EXPECT_EQ(cachetest::clearCalls[0].path, legacy);
    EXPECT_EQ(cachetest::clearCalls[1].kind, "epub");
    EXPECT_EQ(cachetest::clearCalls[1].path, unified);
    EXPECT_FALSE(Storage.exists(legacy.c_str()));
    EXPECT_FALSE(Storage.exists((unified + "/sections").c_str()));
    EXPECT_FALSE(Storage.exists((unified + "/text.manifest").c_str()));
    EXPECT_FALSE(Storage.exists(unified.c_str()));
    expectGlobalsUnchanged();
  }
}

TEST_F(BookCacheUtilsTest, ClearEpubAndXtcDoesNotDispatchTextNamespaces) {
  for (const auto* book : {"/Books/a.epub", "/Books/a.xtc"}) {
    const std::string cache = getBookCachePath(book);
    const std::string other = Txt(book, "/.crosspoint").getCachePath();
    seedCache(cache);
    seedCache(other);
    const auto unchanged = *fake::files[other + "/progress.bin"];
    cachetest::clearCalls.clear();
    fake::mutations.clear();
    clearBookCache(book);
    ASSERT_EQ(cachetest::clearCalls.size(), 1u);
    EXPECT_EQ(cachetest::clearCalls[0].path, cache);
    EXPECT_FALSE(Storage.exists(cache.c_str()));
    EXPECT_EQ(*fake::files[other + "/progress.bin"], unchanged);
    expectGlobalsUnchanged();
  }
}

TEST_F(BookCacheUtilsTest, DirectoryMaintenancePreservesPositionsAndRejectsBroadTargets) {
  const std::string cache = getBookCachePath("/Books/keep.epub");
  const auto retained = seedCache(cache);
  ASSERT_TRUE(invalidateBookCacheDirectory(cache));
  expectRetained(cache, retained);
  const auto before = fake::snapshot();
  fake::mutations.clear();
  for (const auto* path :
       {"", "/", "/.crosspoint", "/.crosspoint/", "/.crosspoint/bookmarks", "/.crosspoint/epub_../book",
        "/.crosspoint/epub_123/sections", "/.crosspoint/epub_123\\book", "/other/epub_123"}) {
    EXPECT_FALSE(invalidateBookCacheDirectory(path)) << path;
  }
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
  expectGlobalsUnchanged();
}

TEST_F(BookCacheUtilsTest, AdditionalTextCacheRelocatesEntireUnifiedTreeNotLegacyCache) {
  for (const auto* extension : {"txt", "md"}) {
    const std::string oldBook = std::string("/Old/book.") + extension;
    const std::string newBook = std::string("/New/renamed.") + extension;
    const std::string oldCache = Epub(oldBook, "/.crosspoint").getCachePath();
    const std::string newCache = Epub(newBook, "/.crosspoint").getCachePath();
    const std::string legacy = Txt(oldBook, "/.crosspoint").getCachePath();
    ASSERT_NE(oldCache, newCache);
    seedCache(oldCache);
    seedCache(legacy);
    const auto before = fake::snapshot();
    fake::mutations.clear();
    relocateAdditionalTextCache(oldBook, newBook);
    EXPECT_FALSE(Storage.exists(oldCache.c_str()));
    ASSERT_TRUE(Storage.exists(newCache.c_str()));
    for (const auto& [path, node] : before) {
      if (path == oldCache || path.starts_with(oldCache + "/")) {
        const std::string moved = newCache + path.substr(oldCache.size());
        ASSERT_TRUE(fake::files.contains(moved));
        EXPECT_EQ(*fake::files[moved], node);
      } else {
        ASSERT_TRUE(fake::files.contains(path));
        EXPECT_EQ(*fake::files[path], node);
      }
    }
    ASSERT_EQ(fake::mutations.size(), 1u);
    EXPECT_EQ(fake::mutations[0].operation, "rename");
    EXPECT_TRUE(Storage.exists(legacy.c_str()));
    expectGlobalsUnchanged();
  }
}

TEST_F(BookCacheUtilsTest, AdditionalTextCacheConflictNeverOverwritesEitherTree) {
  const std::string oldBook = "/Books/old.txt", newBook = "/Books/new.md";
  const std::string oldCache = Epub(oldBook, "/.crosspoint").getCachePath();
  const std::string newCache = Epub(newBook, "/.crosspoint").getCachePath();
  seedCache(oldCache);
  seedCache(newCache);
  fake::add(newCache + "/progress.bin", "destination user data");
  const auto before = fake::snapshot();
  fake::mutations.clear();
  relocateAdditionalTextCache(oldBook, newBook);
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());  // existing destination stops before rename
  expectGlobalsUnchanged();
}

TEST_F(BookCacheUtilsTest, EmptyDestinationDirectoryIsStillAConflict) {
  const std::string oldBook = "/Books/a.txt", newBook = "/Books/b.txt";
  seedCache(Epub(oldBook, "/.crosspoint").getCachePath());
  Storage.mkdir(Epub(newBook, "/.crosspoint").getCachePath().c_str());
  const auto before = fake::snapshot();
  fake::mutations.clear();
  relocateAdditionalTextCache(oldBook, newBook);
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
}

TEST_F(BookCacheUtilsTest, FailedRenameKeepsSourceAndDoesNotCreateDestination) {
  const std::string oldBook = "/Books/source.md", newBook = "/Books/destination.txt";
  seedCache(Epub(oldBook, "/.crosspoint").getCachePath());
  const auto before = fake::snapshot();
  fake::mutations.clear();
  fake::failRename = 0;
  relocateAdditionalTextCache(oldBook, newBook);
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_FALSE(Storage.exists(Epub(newBook, "/.crosspoint").getCachePath().c_str()));
  expectGlobalsUnchanged();
}

TEST_F(BookCacheUtilsTest, SamePathMissingSourceAndUnsupportedRelocationsAreNoOps) {
  const std::string source = "/Books/source.txt";
  seedCache(Epub(source, "/.crosspoint").getCachePath());
  const auto before = fake::snapshot();
  fake::mutations.clear();
  relocateAdditionalTextCache(source, source);
  relocateAdditionalTextCache("/Books/missing.md", "/Books/new.txt");
  relocateAdditionalTextCache(source, "/Books/new.epub");
  relocateAdditionalTextCache("/Books/old.epub", "/Books/new.txt");
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
  expectGlobalsUnchanged();
}
}  // namespace
