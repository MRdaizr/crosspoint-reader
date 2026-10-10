#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "JsonResponse.h"
#include "LibraryQuery.h"
#include "LibraryWebApi.h"
#include "WebServer.h"

namespace {
constexpr char INDEX[] = "/.crosspoint/library.idx";

std::vector<std::string> paths(const JsonResponse& reply) {
  std::vector<std::string> result;
  for (const auto& item : reply.items) {
    const auto found = item.find("path");
    if (found != item.end()) result.push_back(found->second);
  }
  return result;
}

class LibraryWebApiTest : public ::testing::Test {
 protected:
  WebServer server;
  void SetUp() override {
    fake::reset();
    bookMetadata.clear();
    SETTINGS.libraryUseMetadata = 1;
    book("/Books/original.epub", "Original Library", "Shelf Author", 10);
    build();  // also clears the production dirty state left by the prior test
    registerLibraryWebApi(server);
  }
  void book(const std::string& path, const std::string& title, const std::string& author, uint32_t time = 1) {
    fake::add(path, "epub bytes", time);
    bookMetadata[path] = {title, author, true};
  }
  void build() {
    library::BuildStats stats;
    ASSERT_TRUE(library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0));
  }
  JsonResponse get(const WebServer::Arguments& args = {}) {
    EXPECT_TRUE(server.request("/api/library", HTTP_GET, args));
    auto reply = ResponseReader(server.body).parse();
    EXPECT_TRUE(reply.valid) << server.body;
    return reply;
  }
  JsonResponse readOnlyGet(const WebServer::Arguments& args = {}) {
    const auto before = fake::snapshot();
    const unsigned parsed = fake::parses;
    fake::mutations.clear();
    auto reply = get(args);
    EXPECT_TRUE(fake::mutations.empty());
    EXPECT_EQ(fake::snapshot(), before);
    EXPECT_EQ(fake::parses, parsed);
    return reply;
  }
  std::vector<std::string> devicePaths(const std::string& query, library::SortOrder order, uint16_t offset = 0,
                                       uint16_t limit = 32) {
    library::LibraryIndexFile index;
    library::LibraryQuery engine;
    library::QueryPage page;
    std::vector<std::string> result;
    EXPECT_TRUE(index.open(INDEX));
    EXPECT_TRUE(engine.run(index, query, order, offset, limit, page));
    for (uint16_t i = 0; i < page.count; ++i) {
      library::ClixRecord record;
      std::string path;
      EXPECT_TRUE(index.readRecord(page.ordinals[i], record));
      EXPECT_TRUE(index.readPath(record, path));
      result.push_back(path);
    }
    return result;
  }
  void seedSearch() {
    fake::reset();
    bookMetadata.clear();
    book("/Raw/甲.epub", "海边的夏天", "夏目 漱石", 11);
    book("/Raw/乙.epub", "夏天的故事", "森 见", 9);
    book("/Raw/english.epub", "Éclair Sunset", "Alice Writer", 7);
    book("/Raw/not-prefix.epub", "Xsunset story", "Other Person", 5);
    build();
  }
};

TEST_F(LibraryWebApiTest, RegistersOnlyGetAndExplicitPostHandlers) {
  EXPECT_EQ(server.routes.size(), 2u);
  EXPECT_TRUE(server.routes.contains({"/api/library", HTTP_GET}));
  EXPECT_TRUE(server.routes.contains({"/api/library/rebuild", HTTP_POST}));
  EXPECT_FALSE(server.request("/api/library/rebuild", HTTP_GET));
  EXPECT_FALSE(server.request("/api/library/status", HTTP_GET));
}

TEST_F(LibraryWebApiTest, GetIsReadOnlyEvenWithDirtyMarkerAndChangedBook) {
  ASSERT_TRUE(library::markLibraryIndexDirty());
  book("/Books/original.epub", "Changed title", "Changed Author", 20);
  const auto reply = readOnlyGet();
  EXPECT_EQ(server.status, 200);
  EXPECT_EQ(reply.get("dirty"), "true");
  ASSERT_EQ(reply.items.size(), 1u);
  EXPECT_EQ(reply.items[0].at("title"), "Original Library");
  EXPECT_EQ(server.headers["Cache-Control"], "no-store");
  EXPECT_EQ(server.contentLength, CONTENT_LENGTH_UNKNOWN);
  EXPECT_TRUE(server.terminated);
}

TEST_F(LibraryWebApiTest, MissingIndexDoesNotAutoBuildOrPromoteBackup) {
  ASSERT_TRUE(Storage.rename(INDEX, "/.crosspoint/library.bak"));
  const auto reply = readOnlyGet();
  EXPECT_EQ(server.status, 503);
  EXPECT_EQ(reply.get("error"), "library_unavailable");
  EXPECT_FALSE(Storage.exists(INDEX));
  EXPECT_TRUE(Storage.exists("/.crosspoint/library.bak"));
}

TEST_F(LibraryWebApiTest, MissingIndexWithoutBackupDoesNotAutoBuild) {
  ASSERT_TRUE(Storage.remove(INDEX));
  const auto reply = readOnlyGet();
  EXPECT_EQ(server.status, 503);
  EXPECT_EQ(reply.get("error"), "library_unavailable");
  EXPECT_FALSE(Storage.exists(INDEX));
}

TEST_F(LibraryWebApiTest, MetadataSettingMismatchOnlyChangesDirtyResponse) {
  const auto old = fake::files[INDEX]->bytes;
  EXPECT_EQ(readOnlyGet().get("dirty"), "false");
  SETTINGS.libraryUseMetadata = 0;
  EXPECT_EQ(readOnlyGet().get("dirty"), "true");
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  SETTINGS.libraryUseMetadata = 1;
  EXPECT_EQ(readOnlyGet().get("dirty"), "false");
}

TEST_F(LibraryWebApiTest, CjkSubstringAndEnglishPrefixesMatchDeviceQuery) {
  seedSearch();
  for (const auto* term : {"边的", "夏天", "漱", "écl", "ECL", "sun", "sun wr", "clai", "不存在"}) {
    SCOPED_TRACE(term);
    const auto reply = readOnlyGet({{"query", term}, {"sort", "title"}, {"direction", "asc"}});
    EXPECT_EQ(server.status, 200);
    EXPECT_EQ(paths(reply), devicePaths(term, library::SortOrder::TitleAsc));
  }
  EXPECT_EQ(paths(get({{"query", "边的"}})), std::vector<std::string>{"/Raw/甲.epub"});
  EXPECT_EQ(paths(get({{"query", "sun"}})), std::vector<std::string>{"/Raw/english.epub"});
  EXPECT_TRUE(paths(get({{"query", "clai"}})).empty());  // English is not an infix search
}

TEST_F(LibraryWebApiTest, AllSortDirectionEnumsMatchDeviceAndReverseTheirOrder) {
  seedSearch();
  struct OrderCase {
    const char* name;
    library::SortOrder asc, desc;
  };
  const OrderCase cases[] = {{"title", library::SortOrder::TitleAsc, library::SortOrder::TitleDesc},
                             {"author", library::SortOrder::AuthorAsc, library::SortOrder::AuthorDesc},
                             {"recent", library::SortOrder::RecentAsc, library::SortOrder::RecentDesc}};
  for (const auto& entry : cases) {
    SCOPED_TRACE(entry.name);
    const auto ascending = paths(readOnlyGet({{"sort", entry.name}, {"direction", "asc"}}));
    auto descending = paths(readOnlyGet({{"sort", entry.name}, {"direction", "desc"}}));
    EXPECT_EQ(ascending, devicePaths("", entry.asc));
    EXPECT_EQ(descending, devicePaths("", entry.desc));
    std::reverse(descending.begin(), descending.end());
    EXPECT_EQ(ascending, descending);
  }
  EXPECT_EQ(paths(readOnlyGet()), devicePaths("", library::SortOrder::RecentDesc));
}

TEST_F(LibraryWebApiTest, InvalidSortAndDirectionRejectWithoutStorageMutation) {
  for (const auto* value : {"", "TITLE", "filename", "recent;delete", "0"}) {
    SCOPED_TRACE(value);
    EXPECT_EQ(readOnlyGet({{"sort", value}}).get("error"), "invalid_sort");
    EXPECT_EQ(server.status, 400);
  }
  for (const auto* value : {"", "ASC", "ascending", "-1", "desc\n"}) {
    SCOPED_TRACE(value);
    EXPECT_EQ(readOnlyGet({{"direction", value}}).get("error"), "invalid_sort");
    EXPECT_EQ(server.status, 400);
  }
}

TEST_F(LibraryWebApiTest, PaginationZeroOffsetMaxPageAndBeyondEnd) {
  fake::reset();
  bookMetadata.clear();
  SETTINGS.libraryUseMetadata = 0;
  for (unsigned i = 0; i < 36; ++i) {
    char name[40];
    std::snprintf(name, sizeof(name), "/Books/book%04u.txt", i);
    fake::add(name, "plain", i + 1);
  }
  build();
  auto reply = readOnlyGet({{"offset", "0"}, {"limit", "32"}});
  EXPECT_EQ(server.status, 200);
  EXPECT_EQ(reply.get("offset"), "0");
  EXPECT_EQ(reply.get("limit"), "32");
  EXPECT_EQ(reply.get("count"), "32");
  EXPECT_EQ(reply.get("total"), "36");
  EXPECT_EQ(paths(reply), devicePaths("", library::SortOrder::RecentDesc, 0, 32));
  reply = readOnlyGet({{"query", "book"}, {"offset", "32"}, {"limit", "32"}});
  EXPECT_EQ(reply.get("count"), "4");
  EXPECT_EQ(reply.get("total"), "36");
  EXPECT_EQ(paths(reply), devicePaths("book", library::SortOrder::RecentDesc, 32, 32));
  for (const auto* offset : {"36", "4096"}) {
    reply = readOnlyGet({{"offset", offset}, {"limit", "1"}});
    EXPECT_EQ(server.status, 200);
    EXPECT_EQ(reply.get("count"), "0");
    EXPECT_EQ(reply.get("total"), "36");
    EXPECT_TRUE(reply.items.empty());
  }
  EXPECT_EQ(readOnlyGet().get("limit"), "16");
}

TEST_F(LibraryWebApiTest, InvalidPaginationAndOverflowingNumbersAreRejected) {
  for (const auto* value : {"0", "33", "65536", "4294967296", "-1", "+1", "1.0", "1e1", " 1", "1x", ""}) {
    SCOPED_TRACE(value);
    EXPECT_EQ(readOnlyGet({{"limit", value}}).get("error"), "invalid_pagination");
    EXPECT_EQ(server.status, 400);
  }
  for (const auto* value : {"4097", "65536", "4294967296", "-1", "+1", "1.0", "1e1", " 1", "1x", ""}) {
    SCOPED_TRACE(value);
    EXPECT_EQ(readOnlyGet({{"offset", value}}).get("error"), "invalid_pagination");
    EXPECT_EQ(server.status, 400);
  }
  for (const auto& value : {std::string(8192, '9'), std::string(8192, '0') + "1"}) {
    EXPECT_EQ(readOnlyGet({{"offset", value}}).get("error"), "invalid_pagination");
    EXPECT_EQ(server.status, 400);
    EXPECT_EQ(readOnlyGet({{"limit", value}}).get("error"), "invalid_pagination");
    EXPECT_EQ(server.status, 400);
  }
}

TEST_F(LibraryWebApiTest, QueryLimitIsUtf8ByteBounded) {
  EXPECT_EQ(readOnlyGet({{"query", std::string(128, 'a')}}).get("total"), "0");
  EXPECT_EQ(server.status, 200);
  EXPECT_EQ(readOnlyGet({{"query", std::string(129, 'a')}}).get("error"), "query_too_long");
  EXPECT_EQ(server.status, 400);
  std::string cjk;
  for (int i = 0; i < 43; ++i) cjk += "猫";
  EXPECT_EQ(readOnlyGet({{"query", cjk}}).get("error"), "query_too_long");
  EXPECT_EQ(server.status, 400);
}

TEST_F(LibraryWebApiTest, JsonEscapesControlsQuotesAndBackslashesAcrossChunks) {
  fake::reset();
  bookMetadata.clear();
  const std::string rawPath = "/Books/" + std::string(180, '"') + "-raw\\name.epub";
  std::string title = "你好\"\\";
  for (char c = 1; c < 32; ++c) title.push_back(c);
  title += std::string(170, '"');
  book(rawPath, title, "O\"Connor\\Name");
  build();
  const auto reply = readOnlyGet();
  ASSERT_EQ(server.status, 200);
  ASSERT_TRUE(reply.valid);
  ASSERT_EQ(reply.items.size(), 1u);
  EXPECT_EQ(reply.items[0].at("title"), title);
  EXPECT_EQ(reply.items[0].at("path"), rawPath);
  EXPECT_NE(server.body.find("\\u0001"), std::string::npos);
  EXPECT_NE(server.body.find("\\u001f"), std::string::npos);
  EXPECT_NE(server.body.find("\\\""), std::string::npos);
  EXPECT_NE(server.body.find("\\\\"), std::string::npos);
  EXPECT_NE(server.body.find("你好"), std::string::npos);
  EXPECT_GT(server.chunks.size(), 15u);
  EXPECT_TRUE(server.terminated);
}

TEST_F(LibraryWebApiTest, CorruptRecordsFailBeforeAnyHttp200) {
  const auto original = fake::files[INDEX]->bytes;
  library::ClixHeader header;
  std::memcpy(&header, original.data(), sizeof(header));
  library::ClixRecord record;
  std::memcpy(&record, original.data() + library::recordOffset(header, 0), sizeof(record));
  for (int corruption = 0; corruption < 4; ++corruption) {
    SCOPED_TRACE(corruption);
    auto broken = record;
    if (corruption == 0) broken.nameOff = std::numeric_limits<uint32_t>::max();
    if (corruption == 1) broken.nameOff = header.nameLen - 4;
    if (corruption == 2) broken.folderId = header.folderCount;
    if (corruption == 3) broken.metadataStatus = 255;
    fake::files[INDEX]->bytes = original;
    std::memcpy(fake::files[INDEX]->bytes.data() + library::recordOffset(header, 0), &broken, sizeof(broken));
    EXPECT_EQ(readOnlyGet().get("error"), "library_read_failed");
    EXPECT_EQ(server.status, 503);
    EXPECT_EQ(std::count(server.responseCodes.begin(), server.responseCodes.end(), 200), 0);
    EXPECT_FALSE(server.terminated);
  }
}

TEST_F(LibraryWebApiTest, InvalidPermutationAndTruncatedIndexFailBeforeHttp200) {
  const auto old = fake::files[INDEX]->bytes;
  library::ClixHeader header;
  std::memcpy(&header, old.data(), sizeof(header));
  const uint16_t invalid = 0xFFFF;
  std::memcpy(fake::files[INDEX]->bytes.data() + library::arrivalOrderOffset(header, 0), &invalid, sizeof(invalid));
  EXPECT_EQ(readOnlyGet().get("error"), "library_read_failed");
  EXPECT_EQ(server.status, 503);
  fake::files[INDEX]->bytes.resize(12);
  EXPECT_EQ(readOnlyGet().get("error"), "library_unavailable");
  EXPECT_EQ(server.status, 503);
  EXPECT_EQ(std::count(server.responseCodes.begin(), server.responseCodes.end(), 200), 0);
}

TEST_F(LibraryWebApiTest, ReadAndRequestAllocationFailuresDoNotSend200) {
  fake::failRead = 0;
  EXPECT_EQ(readOnlyGet().get("error"), "library_unavailable");
  EXPECT_EQ(server.status, 503);
  fake::failAlloc = 0;
  EXPECT_EQ(readOnlyGet().get("error"), "out_of_memory");
  EXPECT_EQ(server.status, 503);
  EXPECT_EQ(std::count(server.responseCodes.begin(), server.responseCodes.end(), 200), 0);
}

TEST_F(LibraryWebApiTest, DirectHandlersRejectWrongMethods) {
  fake::mutations.clear();
  server.requestMethod = HTTP_POST;
  handleLibraryGet(server);
  EXPECT_EQ(server.status, 405);
  server.resetResponse();
  server.requestMethod = HTTP_GET;
  handleLibraryRebuild(server);
  EXPECT_EQ(server.status, 405);
  EXPECT_EQ(ResponseReader(server.body).parse().get("error"), "method_not_allowed");
  EXPECT_TRUE(fake::mutations.empty());
}

struct BuildContext {
  WebServer* server = nullptr;
  library::BuildPhase phase = library::BuildPhase::Prepare;
  unsigned calls = 0;
  bool disconnect = false;
  uint16_t completed = 0, total = 0;
};
void progress(void* opaque, library::BuildPhase phase, uint16_t completed, uint16_t total) {
  auto& context = *static_cast<BuildContext*>(opaque);
  context.phase = phase;
  context.completed = completed;
  context.total = total;
  ++context.calls;
  if (context.disconnect && phase == library::BuildPhase::Walk && completed > 0) context.server->client().stop();
}
bool cancelAfterOneParse(void* opaque) {
  return static_cast<BuildContext*>(opaque)->phase == library::BuildPhase::Walk && fake::parses >= 1;
}

TEST_F(LibraryWebApiTest, PostCancellationRetainsIndexAndDirtyMarker) {
  // Two changed books ensure the cancellation checkpoint is inside Walk,
  // rather than testing a phase-specific predicate after the last book ended.
  book("/Books/second.epub", "Second book", "Second Author", 11);
  build();
  const auto old = fake::files[INDEX]->bytes;
  fake::files["/Books/original.epub"]->time++;
  fake::files["/Books/second.epub"]->time++;
  fake::parses = 0;
  ASSERT_TRUE(library::markLibraryIndexDirty());
  BuildContext context;
  registerLibraryWebApi(server, {&context, cancelAfterOneParse, progress});
  ASSERT_TRUE(server.request("/api/library/rebuild", HTTP_POST));
  EXPECT_EQ(server.status, 503);
  EXPECT_EQ(ResponseReader(server.body).parse().get("error"), "rebuild_cancelled");
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  EXPECT_TRUE(library::isLibraryIndexDirty());
  EXPECT_TRUE(Storage.exists("/.crosspoint/library.resume"));
  EXPECT_FALSE(library::isLibraryIndexBuilding());
  EXPECT_GT(context.calls, 0u);
}

TEST_F(LibraryWebApiTest, DisconnectedPostClientRetainsIndexWithoutStartingSdWork) {
  const auto before = fake::snapshot();
  fake::mutations.clear();
  server.client().stop();
  ASSERT_TRUE(server.request("/api/library/rebuild", HTTP_POST));
  EXPECT_EQ(server.status, 503);
  EXPECT_EQ(ResponseReader(server.body).parse().get("error"), "rebuild_cancelled");
  EXPECT_EQ(fake::snapshot(), before);
  EXPECT_TRUE(fake::mutations.empty());
  EXPECT_FALSE(library::isLibraryIndexBuilding());
}

TEST_F(LibraryWebApiTest, ClientDisconnectDuringStreamedWalkRetainsPreviousIndex) {
  fake::reset();
  bookMetadata.clear();
  for (int i = 0; i < 40; ++i) book("/Books/" + std::to_string(i) + ".epub", "Title", "Author");
  build();
  const auto old = fake::files[INDEX]->bytes;
  for (auto& [path, node] : fake::files)
    if (path.ends_with(".epub")) ++node->time;
  fake::parses = 0;
  BuildContext context;
  context.server = &server;
  context.disconnect = true;
  registerLibraryWebApi(server, {&context, nullptr, progress});
  ASSERT_TRUE(server.request("/api/library/rebuild", HTTP_POST));
  EXPECT_TRUE(server.client().stopped);
  EXPECT_EQ(server.status, 503);
  EXPECT_EQ(ResponseReader(server.body).parse().get("error"), "rebuild_cancelled");
  EXPECT_GT(fake::parses, 0u);
  EXPECT_EQ(fake::files[INDEX]->bytes, old);
  EXPECT_FALSE(library::isLibraryIndexBuilding());
}

TEST_F(LibraryWebApiTest, PostUsesCurrentMetadataSettingAndCopiesRegistrationCallbacks) {
  SETTINGS.libraryUseMetadata = 0;
  fake::parses = 0;
  BuildContext context;
  library::BuildCallbacks callbacks{&context, nullptr, progress};
  registerLibraryWebApi(server, callbacks);
  callbacks = {};  // registered closure owns the struct, not this temporary
  ASSERT_TRUE(server.request("/api/library/rebuild", HTTP_POST));
  const auto reply = ResponseReader(server.body).parse();
  ASSERT_TRUE(reply.valid);
  EXPECT_EQ(server.status, 200);
  EXPECT_EQ(reply.get("books"), "1");
  EXPECT_EQ(reply.get("parsed"), "0");
  EXPECT_EQ(reply.get("degraded"), "false");
  EXPECT_EQ(context.phase, library::BuildPhase::Complete);
  EXPECT_EQ(context.completed, 1);
  EXPECT_EQ(context.total, 1);
  EXPECT_GT(context.calls, 0u);
  EXPECT_EQ(readOnlyGet().get("dirty"), "false");
  library::LibraryIndexFile index;
  ASSERT_TRUE(index.open(INDEX));
  EXPECT_EQ(index.header().metadataEnabled, 0);
}

TEST_F(LibraryWebApiTest, BusyGetAndNestedPostReturn409WithoutNestedMutations) {
  struct BusyContext {
    WebServer get, post;
    bool checked = false;
  } context;
  const library::BuildCallbacks callbacks{
      &context, [](void*) { return true; },
      [](void* opaque, library::BuildPhase, uint16_t, uint16_t) {
        auto& state = *static_cast<BusyContext*>(opaque);
        const auto before = fake::snapshot();
        fake::mutations.clear();
        state.get.requestMethod = HTTP_GET;
        handleLibraryGet(state.get);
        state.post.requestMethod = HTTP_POST;
        handleLibraryRebuild(state.post);
        EXPECT_EQ(state.get.status, 409);
        EXPECT_EQ(state.post.status, 409);
        EXPECT_EQ(ResponseReader(state.get.body).parse().get("error"), "library_busy");
        EXPECT_EQ(ResponseReader(state.post.body).parse().get("error"), "library_busy");
        EXPECT_EQ(fake::snapshot(), before);
        EXPECT_TRUE(fake::mutations.empty());
        state.checked = true;
      }};
  registerLibraryWebApi(server, callbacks);
  ASSERT_TRUE(server.request("/api/library/rebuild", HTTP_POST));
  EXPECT_TRUE(context.checked);
  EXPECT_FALSE(library::isLibraryIndexBuilding());
}
}  // namespace
