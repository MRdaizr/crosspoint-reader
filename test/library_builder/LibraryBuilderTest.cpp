#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "Epub.h"
#include "LibraryBuilder.h"
#include "LibraryIndexFile.h"
#include "LibraryQuery.h"

using namespace library;

namespace {

constexpr char INDEX[] = "/.crosspoint/library.idx";

std::string numbered(const char* prefix, const unsigned value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%s%04u", prefix, value);
  return text;
}

std::string pathAt(LibraryIndexFile& index, const SortOrder order, const uint16_t row) {
  const uint16_t ordinal = index.ordinalForRow(order, row);
  if (ordinal == 0xFFFF) return {};
  ClixRecord record{};
  if (!index.readRecord(ordinal, record)) return {};
  std::string path;
  return index.readPath(record, path) ? path : std::string();
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  BuildStats stats;

  void SetUp() override {
    fake::reset();
    bookMetadata.clear();
    fake::add("/a.epub");
    fake::add("/b.epub");
  }

  void initial() { ASSERT_TRUE(buildLibraryIndex("/", stats, true)); }
};

}  // namespace

TEST_F(LibraryBuilderTest, UnchangedRebuildReusesMetadataAndDoesNotReplaceIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  EXPECT_EQ(stats.parsed, 0);
  EXPECT_EQ(stats.metadataReused, 2);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, FolderHeavyUnchangedReconciliationIoScalesLinearly) {
  const auto measure = [this](const unsigned count) {
    fake::reset();
    bookMetadata.clear();
    for (unsigned i = 0; i < count; i++) {
      fake::add("/folder" + numbered("", i) + "/book.txt");
    }
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "initial build failed for " << count << " books";
      return 0u;
    }
    fake::resetIoCounters();
    if (!buildLibraryIndex("/", stats, false)) {
      ADD_FAILURE() << "unchanged build failed for " << count << " books";
      return 0u;
    }
    EXPECT_EQ(stats.metadataReused, count);
    EXPECT_FALSE(stats.indexReplaced);
    return fake::reads + fake::seeks;
  };

  const unsigned smallIo = measure(128);
  const unsigned largeIo = measure(256);
  EXPECT_LT(largeIo, smallIo * 3u);
}

TEST_F(LibraryBuilderTest, DirectoryEntriesAreEnumeratedOnce) {
  fake::add("/folder/c.txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(fake::directoryEntriesByPath["/a.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/b.epub"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder"], 1u);
  EXPECT_EQ(fake::directoryEntriesByPath["/folder/c.txt"], 1u);
}

TEST_F(LibraryBuilderTest, DirectoryResumeFailureRetainsPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::add("/aa-folder/c.txt");
  fake::failDirectorySeek = true;

  EXPECT_FALSE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, StagingAndIndexWritesAreBatched) {
  fake::reset();
  for (unsigned i = 0; i < 128; i++) fake::add("/book" + numbered("", i) + ".txt");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_LT(fake::writesByPath["/.crosspoint/library.stage"], 64u);
  EXPECT_LT(fake::writesByPath["/.crosspoint/library.new"], 32u);
}

TEST_F(LibraryBuilderTest, ParentDuplicateTrackingSurvivesDirectoryRecursion) {
  fake::add("/folder/c.txt");
  fake::duplicateDirectoryEntry("/a.epub");

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));

  EXPECT_EQ(stats.books, 3);
  EXPECT_EQ(stats.duplicatesDropped, 1);
}

TEST_F(LibraryBuilderTest, TimestampAndSizeChangesParseOnlyTheChangedBook) {
  initial();
  fake::files["/a.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);

  fake::files["/b.epub"]->bytes.push_back('x');
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 1);
}

TEST_F(LibraryBuilderTest, ZeroTimestampAndFailedExtractionAreNeverFresh) {
  fake::files["/a.epub"]->time = 0;
  bookMetadata["/b.epub"].success = false;
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 2u);
  EXPECT_EQ(stats.metadataReused, 0);
  EXPECT_TRUE(stats.indexReplaced);
}

TEST_F(LibraryBuilderTest, MetadataModeChangesInvalidateCachedMetadata) {
  initial();
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
  index.close();

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::parses, 2u);
}

TEST_F(LibraryBuilderTest, RebuildVotesFromSourceAuthorInsteadOfPriorCanonicalAuthor) {
  fake::add("/c.epub");
  bookMetadata["/a.epub"].author = "Victor Hugo";
  bookMetadata["/b.epub"].author = "Hugo Victor";
  bookMetadata["/c.epub"].author = "Hugo Victor";
  initial();
  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.remove("/c.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  ClixRecord record{};
  std::string author;
  ASSERT_TRUE(index.readRecord(0, record));
  ASSERT_TRUE(index.readAuthor(record, author));
  EXPECT_EQ(author, "Victor Hugo");
}

TEST_F(LibraryBuilderTest, EqualBasenamesInDifferentFoldersReconcileIndependently) {
  fake::add("/one/same.epub");
  fake::add("/two/same.epub");
  bookMetadata["/one/same.epub"].title = "One";
  bookMetadata["/two/same.epub"].title = "Two";
  initial();
  fake::files["/two/same.epub"]->time++;
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(fake::parses, 1u);
  EXPECT_EQ(stats.metadataReused, 3);
}

TEST_F(LibraryBuilderTest, ArrivalOrderFollowsModificationTimeOverDiscoveryOrder) {
  // a and b exist with the default time; c lands with an older timestamp and d
  // with the newest, so file times, not walk or firstSeen order, decide.
  fake::add("/c.epub", "book c", /*time=*/0);
  fake::add("/d.epub", "book d", /*time=*/9);
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/c.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 3), "/d.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentDesc, 0), "/d.epub");
}

TEST_F(LibraryBuilderTest, AddedRemovedMovedAndRenamedBooksKeepArrivalOrder) {
  initial();
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/a.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.remove("/b.epub"));
  ASSERT_TRUE(Storage.rename("/a.epub", "/moved.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.removed, 1);
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/moved.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
  index.close();

  ASSERT_TRUE(Storage.rename("/moved.epub", "/renamed.epub"));
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.renamed, 1);
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 0), "/renamed.epub");
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 1), "/c.epub");
}

TEST_F(LibraryBuilderTest, WholeFolderRenameWithUniqueSizePreservesArrivalOrder) {
  fake::add("/old/unique.epub", "a uniquely sized book");
  initial();
  ASSERT_TRUE(Storage.mkdir("/new"));
  ASSERT_TRUE(Storage.rename("/old/unique.epub", "/new/unique.epub"));
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats, true));

  EXPECT_EQ(stats.renamed, 1);
  EXPECT_EQ(stats.removed, 0);
  EXPECT_EQ(fake::parses, 1u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, 2), "/new/unique.epub");
}

TEST_F(LibraryBuilderTest, DuplicateDetectionRemainsBoundedAndFindsTrackedKeysAfterTheCap) {
  fake::duplicateDirectoryEntry("/a.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(stats.books, 2);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_FALSE(stats.dedupDegraded);

  fake::reset();
  bookMetadata.clear();
  for (unsigned i = 0; i <= LIBRARY_MAX_DEDUP_KEYS; i++) {
    fake::add("/book" + numbered("", i) + ".txt");
  }
  fake::duplicateDirectoryEntry("/book0000.txt");
  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_EQ(stats.books, LIBRARY_MAX_DEDUP_KEYS + 1);
  EXPECT_EQ(stats.duplicatesDropped, 1);
  EXPECT_TRUE(stats.dedupDegraded);
  EXPECT_LT(fake::delays, 2000u);
}

TEST_F(LibraryBuilderTest, ReadWriteCloseAndAllocationFailuresRetainPreviousIndex) {
  initial();
  const auto old = fake::files[INDEX]->bytes;

  fake::failRead = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failRead = -1;

  fake::files["/a.epub"]->time++;
  fake::failWrite = 0;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failWrite = -1;

  fake::failWritePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  fake::failClosePath = "/.crosspoint/library.new";
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);

  // Two 16-byte prior identities. Fail the fatal reconciliation allocation,
  // not the optional dedup/resume buffers whose call positions can change.
  fake::failAllocBytes = 2 * 16;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::failAllocBytes, 0u);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  fake::failAlloc = -1;

  fake::failRename = 1;
  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, TruncatedPersistedPathHashAbortsAndRetainsTheLiveIndex) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  ClixRecord record{};
  std::memcpy(&record, bytes.data() + recordOffset(header, 0), sizeof(record));
  record.nameOff = header.nameLen - 4;
  std::memcpy(bytes.data() + recordOffset(header, 0), &record, sizeof(record));
  const auto corrupted = bytes;

  EXPECT_FALSE(buildLibraryIndex("/", stats, true));
  EXPECT_EQ(fake::files[INDEX]->bytes, corrupted);
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/library.stage.f"));
}

TEST_F(LibraryBuilderTest, LibrariesPastOldGateAndAtFormatCeilingKeepAllOrders) {
  for (const unsigned count : {513u, static_cast<unsigned>(CLIX_MAX_RECORDS)}) {
    fake::reset();
    bookMetadata.clear();
    std::vector<unsigned> authorOrder(count);
    std::iota(authorOrder.begin(), authorOrder.end(), 0u);
    for (unsigned i = 0; i < count; i++) {
      const std::string path = "/book" + numbered("", i) + ".epub";
      fake::add(path);
      bookMetadata[path].title = numbered("Title ", count - 1 - i);
      bookMetadata[path].author = numbered("Writer ", (i * (count == 513 ? 257u : 2053u)) % count);
    }

    ASSERT_TRUE(buildLibraryIndex("/", stats, true)) << count;
    ASSERT_EQ(stats.books, count);
    EXPECT_FALSE(stats.ranksDegraded);

    if (count == CLIX_MAX_RECORDS) {
      const auto old = fake::files[INDEX]->bytes;
      fake::parses = 0;
      fake::resetIoCounters();
      ASSERT_TRUE(buildLibraryIndex("/", stats, true));
      EXPECT_EQ(fake::parses, 0u);
      EXPECT_EQ(stats.metadataReused, CLIX_MAX_RECORDS);
      EXPECT_FALSE(stats.indexReplaced);
      EXPECT_EQ(fake::files[INDEX]->bytes, old);
      EXPECT_LT(fake::delays, 10000u);
    }

    std::sort(authorOrder.begin(), authorOrder.end(), [count](const unsigned a, const unsigned b) {
      return (a * (count == 513 ? 257u : 2053u)) % count < (b * (count == 513 ? 257u : 2053u)) % count;
    });
    LibraryIndexFile index;
    ASSERT_TRUE(index.open(INDEX));
    for (uint16_t row = 0; row < count; row++) {
      EXPECT_EQ(pathAt(index, SortOrder::RecentAsc, row), "/book" + numbered("", row) + ".epub") << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, row), "/book" + numbered("", count - 1 - row) + ".epub")
          << count << ':' << row;
      EXPECT_EQ(pathAt(index, SortOrder::AuthorAsc, row), "/book" + numbered("", authorOrder[row]) + ".epub")
          << count << ':' << row;
    }
  }
}

TEST_F(LibraryBuilderTest, SortAllocationFailureProducesValidDegradedIndex) {
  fake::reset();
  for (unsigned i = 0; i < 513; i++) fake::add("/book" + numbered("", i) + ".txt");
  fake::failAllocBytes = 14 * 513;  // title SortKey array, independent of scratch allocations

  ASSERT_TRUE(buildLibraryIndex("/", stats, false));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_TRUE(stats.ranksDegraded);
  EXPECT_TRUE(stats.indexReplaced);

  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.bookCount(), 513);
}

TEST_F(LibraryBuilderTest, ExplicitRebuildRepairsDegradedRanksWithoutReparsingUnchangedBooks) {
  fake::failAllocBytes = 14 * 2;
  ASSERT_TRUE(buildLibraryIndex("/", stats));
  ASSERT_TRUE(stats.ranksDegraded);
  fake::parses = 0;

  ASSERT_TRUE(buildLibraryIndex("/", stats));
  EXPECT_TRUE(stats.indexReplaced);
  EXPECT_FALSE(stats.ranksDegraded);
  EXPECT_EQ(fake::parses, 0u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_FALSE(index.ranksDegraded());
}

TEST_F(LibraryBuilderTest, RecoveryRetainsStructurallyValidBackupWithStaleFoldVersion) {
  initial();
  auto& old = fake::files[INDEX]->bytes;
  ClixHeader header;
  std::memcpy(&header, old.data(), sizeof(header));
  header.foldVersion = CLIX_FOLD_VERSION - 1;
  std::memcpy(old.data(), &header, sizeof(header));
  const auto saved = old;
  ASSERT_TRUE(Storage.rename(INDEX, "/.crosspoint/library.bak"));
  fake::add(INDEX, "truncated interrupted replacement");

  ASSERT_TRUE(recoverLibraryIndex());
  EXPECT_EQ(fake::files[INDEX]->bytes, saved);
  LibraryIndexFile index;
  EXPECT_FALSE(index.open(INDEX));
  EXPECT_TRUE(index.openForReconciliation(INDEX));
  const auto firstSeen = index.header().nextFirstSeen;
  index.close();
  ASSERT_TRUE(buildLibraryIndex("/", stats));
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().nextFirstSeen, firstSeen);
  EXPECT_EQ(stats.unchanged, 2);
}

namespace {
struct CancelContext {
  BuildPhase phase = BuildPhase::Prepare;
  BuildPhase cancelPhase = BuildPhase::Walk;
  unsigned parsed = 1;
  unsigned progressCalls = 0;
  uint16_t completed = 0;
  uint16_t total = 0;
};
void progress(void* ctx, BuildPhase phase, uint16_t completed, uint16_t total) {
  auto& state = *static_cast<CancelContext*>(ctx);
  state.phase = phase;
  state.progressCalls++;
  state.completed = completed;
  state.total = total;
}
bool cancel(void* ctx) {
  const auto& state = *static_cast<CancelContext*>(ctx);
  return state.phase == state.cancelPhase && fake::parses >= state.parsed;
}
}  // namespace

TEST_F(LibraryBuilderTest, MetadataDefaultsOn) {
  ASSERT_TRUE(buildLibraryIndex("/", stats));
  EXPECT_EQ(fake::parses, 2u);
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 1);
}

TEST_F(LibraryBuilderTest, CompleteCallbackReportsCompletedTotal) {
  CancelContext context;
  const BuildCallbacks callbacks{&context, nullptr, progress};
  ASSERT_TRUE(buildLibraryIndex("/", stats, true, callbacks));
  EXPECT_EQ(context.phase, BuildPhase::Complete);
  EXPECT_EQ(context.completed, stats.books);
  EXPECT_EQ(context.total, stats.books);
}

TEST_F(LibraryBuilderTest, CancelledWalkRetainsLiveIndexAndReusesCompletedStage) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  fake::files["/a.epub"]->time++;
  fake::files["/b.epub"]->time++;
  fake::parses = 0;
  CancelContext context;
  BuildCallbacks callbacks{&context, cancel, progress};
  EXPECT_FALSE(buildLibraryIndex("/", stats, true, callbacks));
  EXPECT_TRUE(stats.cancelled);
  EXPECT_FALSE(stats.indexReplaced);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  EXPECT_GT(context.progressCalls, 0u);
  EXPECT_TRUE(Storage.exists("/.crosspoint/library.resume"));
  fake::parses = 0;
  ASSERT_TRUE(buildLibraryIndex("/", stats));
  EXPECT_EQ(stats.stagedMetadataReused, 1);
  EXPECT_EQ(fake::parses, 1u);
}

TEST_F(LibraryBuilderTest, CancellationDuringEveryPostWalkPhaseKeepsPreviousIndex) {
  for (const auto phase : {BuildPhase::Reconcile, BuildPhase::Sort, BuildPhase::Emit, BuildPhase::Install}) {
    fake::reset();
    fake::add("/a.epub");
    fake::add("/b.epub");
    initial();
    const auto old = fake::files[INDEX]->bytes;
    fake::files["/a.epub"]->time++;
    CancelContext context;
    context.cancelPhase = phase;
    context.parsed = 0;
    BuildCallbacks callbacks{&context, cancel, progress};
    EXPECT_FALSE(buildLibraryIndex("/", stats, true, callbacks));
    EXPECT_TRUE(stats.cancelled);
    EXPECT_EQ(fake::files[INDEX]->bytes, old);
    EXPECT_FALSE(isLibraryIndexBuilding());
  }
}

TEST_F(LibraryBuilderTest, InterruptedRenameRecoversBackupWithoutRebuild) {
  initial();
  const auto old = fake::files[INDEX]->bytes;
  ASSERT_TRUE(Storage.rename(INDEX, "/.crosspoint/library.bak"));
  ASSERT_TRUE(recoverLibraryIndex());
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
}

TEST_F(LibraryBuilderTest, SharedQueryFindsCjkSubstringAndEnglishWordPrefixesInTitleAndAuthor) {
  bookMetadata["/a.epub"].title = "远方的三体世界";
  bookMetadata["/a.epub"].author = "刘慈欣";
  bookMetadata["/b.epub"].title = "L’Énéide";
  bookMetadata["/b.epub"].author = "Paul Éluard";
  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  LibraryQuery query;
  QueryPage page;
  ASSERT_TRUE(query.run(index, "三体 慈欣", SortOrder::TitleAsc, 0, 32, page));
  EXPECT_EQ(page.total, 1);
  ASSERT_TRUE(query.run(index, "ene elu", SortOrder::AuthorAsc, 0, 32, page));
  EXPECT_EQ(page.total, 1);
  EXPECT_FALSE(query.run(index, "", SortOrder::RecentDesc, 0, 33, page));
  EXPECT_FALSE(query.run(index, "", SortOrder::RecentDesc, 0, 0, page));
}

TEST_F(LibraryBuilderTest, SharedQuerySearchesPastShortSortKeyAndPagesWithoutChangingPaths) {
  bookMetadata["/a.epub"].title = std::string(120, 'a') + " needle";
  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  LibraryQuery query;
  QueryPage page;
  ASSERT_TRUE(query.run(index, "needle", SortOrder::TitleAsc, 0, 1, page));
  ASSERT_EQ(page.count, 1);
  ClixRecord record{};
  ASSERT_TRUE(index.readRecord(page.ordinals[0], record));
  char path[513];
  ASSERT_TRUE(index.readPath(record, path, sizeof(path)));
  EXPECT_STREQ(path, "/a.epub");
  ASSERT_TRUE(query.run(index, "", SortOrder::TitleAsc, 1, 1, page));
  EXPECT_EQ(page.total, 2);
  EXPECT_EQ(page.count, 1);
  ASSERT_TRUE(query.run(index, "", SortOrder::TitleAsc, 4096, 1, page));
  EXPECT_EQ(page.count, 0);
  EXPECT_EQ(page.total, 2);
}

TEST_F(LibraryBuilderTest, SamePrefixTitlesAreSortedBeyondTheFirstFourCjkCharacters) {
  bookMetadata["/a.epub"].title = "共同前缀乙";
  bookMetadata["/b.epub"].title = "共同前缀甲";
  initial();
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  // Unicode byte order: 乙 sorts before 甲. The full key must decide it,
  // irrespective of discovery order, so reverse the metadata and rebuild.
  index.close();
  bookMetadata["/a.epub"].title = "共同前缀甲";
  bookMetadata["/b.epub"].title = "共同前缀乙";
  fake::files["/a.epub"]->time++;
  fake::files["/b.epub"]->time++;
  ASSERT_TRUE(buildLibraryIndex("/", stats));
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(pathAt(index, SortOrder::TitleAsc, 0), "/b.epub");
  EXPECT_EQ(pathAt(index, SortOrder::TitleDesc, 0), "/a.epub");
}

TEST_F(LibraryBuilderTest, FirstSeenCounterCompactsBeforeReservedSentinelAndWrap) {
  initial();
  auto& bytes = fake::files[INDEX]->bytes;
  ClixHeader header{};
  memcpy(&header, bytes.data(), sizeof(header));
  header.nextFirstSeen = 65534;
  memcpy(bytes.data(), &header, sizeof(header));
  fake::add("/c.epub");
  ASSERT_TRUE(buildLibraryIndex("/", stats));
  LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_LT(index.header().nextFirstSeen, 10);
  for (uint16_t i = 0; i < index.bookCount(); ++i) {
    ClixRecord record{};
    ASSERT_TRUE(index.readRecord(i, record));
    EXPECT_NE(record.firstSeen, 65535);
  }
}
