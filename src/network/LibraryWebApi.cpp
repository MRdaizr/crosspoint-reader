#include "LibraryWebApi.h"

#include <LibraryQuery.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "util/TaskWatchdog.h"

namespace {
void error(WebServer& server, const int code, const char* message) {
  LOG_ERR("LIBWEB", "%d: %s", code, message);
  // API error tokens are stable machine-readable values, not device UI text.
  char body[96];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
  server.send(code, "application/json; charset=utf-8", body);
}
bool unsignedArg(WebServer& server, const char* name, const uint16_t fallback, const uint16_t maximum, uint16_t& out) {
  out = fallback;
  if (!server.hasArg(name)) return true;
  const String value = server.arg(name);
  // A uint16 decimal needs at most five bytes. Range checks alone accept an
  // arbitrarily long leading-zero payload, wasting work on attacker input.
  if (value.isEmpty() || value.length() > 5) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    parsed = parsed * 10 + value[i] - '0';
    if (parsed > maximum) return false;  // bounded before next multiplication
  }
  out = parsed;
  return true;
}
void quoted(WebServer& server, const char* text) {
  char chunk[96];
  size_t used = 0;
  server.sendContent("\"", 1);
  resetTaskWatchdogIfSubscribed();
  for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    if (used + 6 >= sizeof(chunk)) {
      server.sendContent(chunk, used);
      resetTaskWatchdogIfSubscribed();
      used = 0;
    }
    if (*p == '\"' || *p == '\\') {
      chunk[used++] = '\\';
      chunk[used++] = *p;
    } else if (*p < 0x20) {
      static constexpr char hex[] = "0123456789abcdef";
      chunk[used++] = '\\';
      chunk[used++] = 'u';
      chunk[used++] = '0';
      chunk[used++] = '0';
      chunk[used++] = hex[*p >> 4];
      chunk[used++] = hex[*p & 15];
    } else
      chunk[used++] = *p;
  }
  if (used) server.sendContent(chunk, used);
  server.sendContent("\"", 1);
}
struct WebScratch {
  library::LibraryIndexFile index;
  library::LibraryQuery query;
  library::QueryPage page;
  library::ClixRecord record{};
  char title[256] = {};
  char author[256] = {};
  char path[513] = {};
  char json[192] = {};
};
struct RebuildContext {
  WebServer& server;
  const library::BuildCallbacks& outer;
};
bool cancelRebuild(void* context) {
  auto& ctx = *static_cast<RebuildContext*>(context);
  return !ctx.server.client().connected() || (ctx.outer.shouldCancel && ctx.outer.shouldCancel(ctx.outer.context));
}
void progressRebuild(void* context, const library::BuildPhase phase, const uint16_t done, const uint16_t total) {
  auto& ctx = *static_cast<RebuildContext*>(context);
  if (ctx.outer.progress) ctx.outer.progress(ctx.outer.context, phase, done, total);
}
}  // namespace

void handleLibraryGet(WebServer& server) {
  if (server.method() != HTTP_GET) {
    error(server, 405, "method_not_allowed");
    return;
  }
  if (library::isLibraryIndexBuilding()) {
    error(server, 409, "library_busy");
    return;
  }
  uint16_t offset = 0, limit = 16;
  if (!unsignedArg(server, "offset", 0, library::CLIX_MAX_RECORDS, offset) ||
      !unsignedArg(server, "limit", 16, library::LIBRARY_PAGE_LIMIT, limit) || !limit) {
    error(server, 400, "invalid_pagination");
    return;
  }
  const String query = server.hasArg("query") ? server.arg("query") : String();
  if (query.length() > library::LIBRARY_QUERY_BYTES) {
    error(server, 400, "query_too_long");
    return;
  }
  const String sort = server.hasArg("sort") ? server.arg("sort") : String("recent");
  const String direction = server.hasArg("direction") ? server.arg("direction") : String("desc");
  if ((sort != "title" && sort != "author" && sort != "recent") || (direction != "asc" && direction != "desc")) {
    error(server, 400, "invalid_sort");
    return;
  }
  const bool descending = direction == "desc";
  const auto order = sort == "title"    ? (descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc)
                     : sort == "author" ? (descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc)
                     : descending       ? library::SortOrder::RecentDesc
                                        : library::SortOrder::RecentAsc;
  // ~3 KiB scratch allocated once for the request, never a full JSON shelf.
  auto scratch = makeUniqueNoThrow<WebScratch>();
  if (!scratch) {
    error(server, 503, "out_of_memory");
    return;
  }
  // GET deliberately does not repair/rebuild files. A missing live path with a
  // backup remains recoverable by the next explicit POST or device entry.
  if (!scratch->index.open(library::libraryIndexPath())) {
    error(server, 503, "library_unavailable");
    return;
  }
  if (!scratch->query.run(scratch->index, std::string_view(query.c_str(), query.length()), order, offset, limit,
                          scratch->page)) {
    error(server, 503, "library_read_failed");
    return;
  }
  // Validate this tiny page before sending headers, so corrupt blobs cannot
  // produce a successful-looking partial response.
  for (uint16_t i = 0; i < scratch->page.count; ++i) {
    if (!scratch->index.readRecord(scratch->page.ordinals[i], scratch->record) ||
        !scratch->query.readDisplay(scratch->index, scratch->record, scratch->title, sizeof(scratch->title),
                                    scratch->author, sizeof(scratch->author)) ||
        !scratch->index.readPath(scratch->record, scratch->path, sizeof(scratch->path))) {
      error(server, 503, "library_read_failed");
      return;
    }
  }
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");
  auto& prefix = scratch->json;
  const bool dirty =
      library::isLibraryIndexDirty() || scratch->index.header().metadataEnabled != (SETTINGS.libraryUseMetadata != 0);
  snprintf(prefix, sizeof(prefix),
           "{\"total\":%u,\"offset\":%u,\"limit\":%u,\"count\":%u,\"dirty\":%s,\"degraded\":%s,\"items\":[",
           scratch->page.total, offset, limit, scratch->page.count, dirty ? "true" : "false",
           scratch->page.degraded ? "true" : "false");
  server.sendContent(prefix, strlen(prefix));
  for (uint16_t i = 0; i < scratch->page.count; ++i) {
    if (!scratch->index.readRecord(scratch->page.ordinals[i], scratch->record) ||
        !scratch->query.readDisplay(scratch->index, scratch->record, scratch->title, sizeof(scratch->title),
                                    scratch->author, sizeof(scratch->author)) ||
        !scratch->index.readPath(scratch->record, scratch->path, sizeof(scratch->path))) {
      server.client().stop();
      return;  // incomplete chunked response must fail
    }
    if (i) server.sendContent(",", 1);
    server.sendContent("{\"title\":", 9);
    quoted(server, scratch->title);
    server.sendContent(",\"author\":", 10);
    quoted(server, scratch->author);
    server.sendContent(",\"path\":", 8);
    quoted(server, scratch->path);
    // modificationTime is packed FAT date/time, never Unix seconds.
    snprintf(prefix, sizeof(prefix), ",\"size\":%u,\"firstSeen\":%u,\"modificationTime\":%u}",
             static_cast<unsigned>(scratch->record.fileSize), scratch->record.firstSeen,
             static_cast<unsigned>(scratch->record.modificationTime));
    server.sendContent(prefix, strlen(prefix));
  }
  server.sendContent("]}", 2);
  server.sendContent("", 0);
}

void handleLibraryRebuild(WebServer& server, const library::BuildCallbacks& outer) {
  if (server.method() != HTTP_POST) {
    error(server, 405, "method_not_allowed");
    return;
  }
  library::BuildStats stats;
  RebuildContext context{server, outer};
  const library::BuildCallbacks callbacks{&context, &cancelRebuild, &progressRebuild};
  if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0, callbacks)) {
    error(server, stats.busy ? 409 : 503,
          stats.busy        ? "library_busy"
          : stats.cancelled ? "rebuild_cancelled"
                            : "rebuild_failed");
    return;
  }
  char response[192];
  snprintf(
      response, sizeof(response),
      "{\"books\":%u,\"parsed\":%u,\"metadataReused\":%u,\"stagedMetadataReused\":%u,\"degraded\":%s,\"capped\":%s}",
      stats.books, stats.parsed, stats.metadataReused, stats.stagedMetadataReused,
      stats.ranksDegraded ? "true" : "false", stats.capped ? "true" : "false");
  server.send(200, "application/json; charset=utf-8", response);
}

void registerLibraryWebApi(WebServer& server, const library::BuildCallbacks& callbacks) {
  server.on("/api/library", HTTP_GET, [&server] { handleLibraryGet(server); });
  server.on("/api/library/rebuild", HTTP_POST, [&server, callbacks] { handleLibraryRebuild(server, callbacks); });
}
