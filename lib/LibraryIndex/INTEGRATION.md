# Library integration contract

This module is ported to the current deb62ab SDK/HAL, not a SDK upgrade. The
shared entry points, translations, web page and parent test registration belong
to the coordinating agent. Home retains Files, Recents, OPDS, file transfer,
Extensions/WeRead and Settings, plus the new Library row. Recent covers, theme
icon policy and Home font prewarming are unchanged.

## Device and settings

ActivityManager.h needs `void goToLibrary();` and `HomeMenuItem::LIBRARY`.
ActivityManager.cpp integration (already supplied centrally during coordination):

```cpp
#include "activities/library/LibraryListActivity.h"
#include <Memory.h>

void ActivityManager::goToLibrary() {
  auto activity = makeUniqueNoThrow<LibraryListActivity>(renderer, mappedInput);
  if (!activity) { LOG_ERR("ACT", "OOM: library activity"); return; }
  replaceActivity(std::move(activity));
}
```

CrossPointSettings needs `uint8_t libraryUseMetadata = 1;`, persisted using the
normal settings registry. The SettingsList toggle is:

```cpp
SettingInfo::Toggle(StrId::STR_LIBRARY_USE_METADATA,
                    &CrossPointSettings::libraryUseMetadata,
                    "libraryUseMetadata", StrId::STR_CAT_READER)
```

Device entry detects missing, dirty or metadata-mode-mismatched indexes and
rebuilds on the next main-loop pass, with progress and logical Back cancellation.
Title/author/recent tabs toggle direction when selected again. Search is a tool
row, followed by an explicit confirmed Rebuild row. Back clears a search first,
then returns Home with Library selected. The device's unfiltered newest-first
tab overlays up to ten Recents and hides their duplicate index rows. HTTP returns
index rows only; `recent` means SD arrival/modification order, not reading history.

## Web API registration and lifecycle

The exact global entry points are in src/network/LibraryWebApi.h:

```cpp
void registerLibraryWebApi(WebServer& server,
                          const library::BuildCallbacks& callbacks = {});
void handleLibraryGet(WebServer& server);
void handleLibraryRebuild(WebServer& server,
                         const library::BuildCallbacks& callbacks = {});
```

In CrossPointWebServer.cpp, after successfully creating the WebServer and before
`server->begin()`:

```cpp
#include "LibraryWebApi.h"
// In begin(), with the existing checked server allocation:
registerLibraryWebApi(*server);
```

This registers GET `/api/library` and POST `/api/library/rebuild`. Register once
per server instance. There is **no extra poll/loop function, background task or
status endpoint**: the existing `server->handleClient()` dispatches both handlers.
POST is a synchronous, yielding builder invocation. Optional callbacks can
display progress/poll cancellation on that task; they must not recursively call
handleClient, navigate an activity, destroy the server or start another build.
The registered callback struct is copied; its context must outlive the server.
POST also cancels when its HTTP client disconnects. If wrapping handlers in an
existing authentication policy, use the two handle functions instead of automatic
registration; never register competing handlers for the same URL.

```cpp
namespace library {
enum class BuildPhase : uint8_t {
  Prepare, Walk, Reconcile, Sort, Emit, Install, Complete
};
struct BuildCallbacks {
  void* context = nullptr;
  bool (*shouldCancel)(void*) = nullptr;
  void (*progress)(void*, BuildPhase, uint16_t completed, uint16_t total) = nullptr;
};
bool buildLibraryIndex(const char* rootPath, BuildStats& stats,
                       bool readMetadata = true,
                       const BuildCallbacks& callbacks = {});
bool recoverLibraryIndex();
bool isLibraryIndexBuilding();
bool markLibraryIndexDirty();
bool isLibraryIndexDirty();
}
```

Discovery's total is zero/unknown; later phases provide approximate work counts.
Complete reports completed == total == books. Cancellation is checked between
streamed work units; watchdog/idle yields occur every 32 units. One OPF metadata
read and a short in-memory prefix sort are indivisible. After the last cancellation
check, transactional renames complete or roll back without accepting cancellation.
Concurrent builds fail with BuildStats.busy instead of touching staging files.

### GET query contract

| Argument | Accepted values / default |
| --- | --- |
| query | UTF-8, at most 128 bytes; default empty |
| sort | `title`, `author`, `recent`; default `recent` |
| direction | `asc`, `desc`; default `desc` |
| offset | integer 0..4096; default 0 |
| limit | integer 1..32; default 16 |

Pagination arguments are at most five ASCII decimal digits; excessively long
leading-zero payloads are rejected as invalid_pagination as well as overflow.

Success is streamed JSON, with only one bounded page materialized:

```json
{
  "total": 2, "offset": 0, "limit": 16, "count": 2,
  "dirty": false, "degraded": false,
  "items": [{
    "title": "Book title", "author": "Author", "path": "/Books/raw name.epub",
    "size": 12345, "firstSeen": 7, "modificationTime": 0
  }]
}
```

`total` is the filtered count, not the current page length. Titles/author names
are display metadata; `path` is the unchanged original SD path. modificationTime
is packed FAT date/time, **not Unix seconds**; zero means unavailable. firstSeen
is a compactable arrival sequence, not a timestamp. `degraded` explicitly warns
that an allocation failure prevented reliable rank sorting. An explicit rebuild
retries sorting even if all book metadata is unchanged.

GET never repairs or rebuilds. It can serve the previous index with dirty=true.
A missing live path returns library_unavailable; explicit POST or device entry
recovers a retained backup. Invalid arguments return 400; a busy builder returns
409; unavailable storage/index, I/O failure or OOM return 503. Bodies have a stable
`error` token: method_not_allowed, invalid_pagination, query_too_long, invalid_sort,
library_busy, out_of_memory, library_unavailable, library_read_failed,
rebuild_cancelled or rebuild_failed.

POST has no required body. Success returns books, parsed, metadataReused,
stagedMetadataReused, degraded and capped. The browser should await this response
and then fetch GET again; it should not poll a nonexistent status route.

### Webpage integration snippet (owned by the web/plugin agent)

```js
async function fetchLibrary(query, sort, direction, offset, signal) {
  const args = new URLSearchParams({query, sort, direction,
                                    offset: String(offset), limit: '16'});
  const response = await fetch('/api/library?' + args, {signal, cache: 'no-store'});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error);
  return data;
}
async function rebuildLibrary(signal) {
  const response = await fetch('/api/library/rebuild', {method: 'POST', signal});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error);
  return data; // then refetch the requested page
}
```

Debounce search, abort superseded GETs, reset offset on query/sort/direction changes,
and use data.total/count for pagination. Request cancellation can abort POST too.
Display dirty/degraded/capped warnings using page translations. Assign metadata to
DOM textContent, never innerHTML; pass item.path unchanged to existing book actions
and URL-encode only at transport boundaries. Do not duplicate filtering in JS:
LibraryQuery is shared by device and HTTP. English uses diacritic-folded word
prefixes; Han/kana/Hangul words use Unicode substrings; multiword terms all match
within title + author. Search reads beyond the record's short 96-byte sort key.

## Mutation and metadata dependencies

The parent's HalStorage MutationCallback calls markLibraryIndexDirty() for book
creation/upload/download/removal/moves/renames and directory changes inside the
recursive StorageLock, after the SDK operation has been serialized. The callback
may perform SD operations but must not take UI/renderer locks. Filter hidden
internal index/cache paths to avoid recursive hooks. Coverage includes successful
openFileForWrite/remove/rename/rmdir/removeDir, Storage.open with write-mode flags,
and successful writeFile. The default callback handles both TXT cache namespaces.
No additional per-transfer dirty hooks were added here. Do not clear a dirty marker
externally: a generation counter retains mutations that occur during a rebuild.
Mode changes are detected from the persisted index header; optional explicit
dirty marking must be value-change guarded.

Required existing/coordinated APIs:

```cpp
uint32_t HalFile::modificationTime(); // packed FAT date/time; 0 = unknown
bool HalFile::truncate(uint64_t length);
bool HalStorage::readFileToString(const char* moduleName, const std::string& path,
                                  size_t cap, std::string& out);
bool HalStorage::replaceFile(const char* tmpPath, const char* path);
bool Epub::loadMetadata(std::string& title, std::string& author);
```

The index builder directly needs modificationTime and metadata-only loadMetadata,
plus existing HAL open/read/write/seek/close/rename/remove operations. It owns its
`.idx/.new/.bak` install protocol; it does not depend on HAL replaceFile. Unknown
timestamps and unsuccessful extraction never count as reusable metadata. Raw book
paths remain separate from metadata. Unchanged successful metadata is reused from
the live index; cancelled/interrupted stages can reuse up to 512 checked entries
using a bounded optional lookup. The live index alone owns firstSeen reconciliation.

## On-disk and memory limits

CLX1 format 2 retains 128-byte records, 4096 books and depth 5 below the scan root.
Fold/rank version 5 supersedes the baseline version 4 prefix-only title ordering;
structurally valid old-fold indexes remain readable for firstSeen reconciliation
and backup recovery, then their metadata/ranks are rebuilt. Titles are stored up
to 255 UTF-8 bytes, authors up to 128; accepted raw basenames/folders are at most
255 bytes each. Over-limit/unreadable entries are counted/skipped, not path-rewritten.

`.crosspoint/library.idx` stays live until the complete `.new` has been flushed,
closed, size-checked and validated. Rename uses `.bak` for rollback/recovery. Stage
files are never promoted to live; `.resume` is only a metadata hint. Sort allocation
failure creates a structurally valid degraded index with an explicit UI/API warning;
fatal I/O/allocation errors keep the previous index/backup recoverable.

All new large buffers are checked nothrow heap allocations to avoid C3 stack
overflow/static DRAM retention. Device display storage is 32 x 385 bytes plus one
1536-byte reusable query scratch; rows are populated only for the visible viewport.
HTTP uses one roughly 3 KiB scratch and bounded JSON chunks, not a full JSON shelf.
Build-only arrays are phase-local: prior identities <=65,536 bytes, optional dedup
8,192 bytes, optional resumed-stage lookup <=8,192 bytes; these are released before
title/author sort-key arrays (<=57,344 bytes each, not simultaneously). Ordinal and
firstSeen arrays are <=8,192 bytes each; stream buffers are 4 KiB. Metadata parser
scratch exists only for the currently changed EPUB, never throughout sorting.

## UI translation keys (coordinator adds YAML, generator owns headers)

| Key | Suggested English |
| --- | --- |
| STR_LIBRARY | Library |
| STR_LIBRARY_USE_METADATA | Use book metadata |
| STR_LIBRARY_TAB_RECENT | Recent |
| STR_LIBRARY_TAB_TITLE | Title |
| STR_LIBRARY_TAB_AUTHOR | Author |
| STR_LIBRARY_SEARCH | Search library |
| STR_LIBRARY_REBUILD | Rebuild library |
| STR_LIBRARY_REBUILD_CONFIRM | Rebuild the library index? |
| STR_LIBRARY_REBUILDING | Rebuilding library |
| STR_LIBRARY_CANCEL_HINT | Cancel |
| STR_LIBRARY_REBUILD_CANCELLED | Rebuild cancelled; previous library retained |
| STR_LIBRARY_REBUILD_FAILED | Rebuild failed; previous library retained |
| STR_LIBRARY_READ_FAILED | Cannot read library |
| STR_LIBRARY_BUSY | Library is busy |
| STR_LIBRARY_SORT_DEGRADED | Sorting unavailable; rebuild to retry |
| STR_LIBRARY_CAPPED | Library limit reached (4096 books) |
| STR_LIBRARY_UNKNOWN_AUTHOR | Unknown author |

## Test registrations and verification

The parent has already registered the four new directories. For reference only:

```cmake
add_subdirectory(library_format)
add_subdirectory(library_text)
add_subdirectory(library_index_file)
add_subdirectory(library_builder)
```

The builder target includes LibraryQuery.cpp. Do not duplicate registrations.
The final scoped host run passed 94 tests: format 18, text 39, index file 7,
builder/query/recovery 30. Host tests intentionally do not define ARDUINO, so the
existing ESP watchdog helper is excluded from those builds.
Tests cover format validation, fold/query semantics, bounded indexed reads,
4096-cap ordering, staged reuse, every cancellation phase, failed I/O/close/rename
and precise fatal-allocation injection, stale-fold backup recovery, degradation
repair, completed progress, CJK same-prefix ordering and firstSeen counter compaction.

Final firmware build is coordinated centrally. On hardware, verify all four
orientations, remapped Back cancellation, unchanged paths after metadata/search,
recents overlay, CJK title/author prewarming, and a cancelled rebuild followed by
successful staged reuse. Check LOG_LEVEL=2 build phase logs, free/max-block heap
and task stack high-water marks at 4096 books; only on-device measurements establish
safe aggregate heap headroom with the active fonts/framebuffer/metadata parser.

## Independent API/cache quality regressions

The coordinator registers test/library_web_api and test/book_cache_utils in the
parent CMake file. Targets are LibraryWebApiTest (21 cases) and BookCacheUtilsTest
(16 cases). Both compile the production .cpp files, not copied implementations.
API tests use an observed in-memory HAL and fake WebServer/String; shared query
results are compared against device LibraryQuery and JSON is decoded independently.
Book cache tests use Epub/Txt/Xtc boundary stubs; they verify production dispatch,
selective invalidation and relocation, not the reader engines' parsing internals.
The parent-owned BookCacheUtils.cpp remains read-only.

Each subdirectory can also configure independently without fetching the simulator:

```sh
cmake -S test/library_web_api -B build/library-web-api-host \
  -DGTEST_SOURCE_DIR=/absolute/path/to/existing/googletest-checkout
cmake --build build/library-web-api-host --target LibraryWebApiTest
ctest --test-dir build/library-web-api-host --output-on-failure

cmake -S test/book_cache_utils -B build/book-cache-utils-host \
  -DGTEST_SOURCE_DIR=/absolute/path/to/existing/googletest-checkout
cmake --build build/book-cache-utils-host --target BookCacheUtilsTest
ctest --test-dir build/book-cache-utils-host --output-on-failure
```

GTEST_SOURCE_DIR is the checkout root (containing googletest/), not that inner
directory. Omit it to use an installed GTest package. On GNU/Clang Linux/WSL,
`-DCROSSPOINT_REGRESSION_SANITIZERS=ON` instruments just these test targets with
ASAN+UBSAN, -O1 -g and frame pointers, plus non-PIE flags to avoid WSL shadow mapping
collisions. Existing parent-wide sanitizer flags work too; keep test/GTest RTTI
settings consistent instead of linking no-RTTI test factories to vptr-instrumented
GTest. Run with ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 and
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1. Do not disable sanitizer diagnostics.
