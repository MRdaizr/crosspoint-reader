#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "ReaderSession.h"
#include "activities/plugins/PluginJobPool.h"
#include "network/ProtectedPaths.h"
#include "util/PluginEventsPolicy.h"
#include "util/PluginHttpPolicy.h"
#include "util/PluginPermissions.h"

static unsigned checks = 0;
#define CHECK(x)                                   \
  do {                                             \
    ++checks;                                      \
    if (!(x)) {                                    \
      std::cerr << __LINE__ << ": " << #x << '\n'; \
      std::exit(1);                                \
    }                                              \
  } while (0)
static void classificationAndPermission() {
  using namespace PluginLocations;
  CHECK(std::string(ROOTS[0]) == "/.crosspoint/plugins");
  CHECK(std::string(ROOTS[1]) == "/plugins");
  CHECK(std::string(ROOTS[2]) == "/.plugins");
  CHECK(classifyDeviceManifest(true, true) == DeviceKind::Catalog);
  CHECK(classifyDeviceManifest(false, true) == DeviceKind::Background);
  CHECK(classifyDeviceManifest(false, false) == DeviceKind::None);
  CHECK(pickerAction(DeviceKind::Background, true) == PickerAction::Readme);
  CHECK(pickerAction(DeviceKind::None, false) == PickerAction::None);
  CHECK(validName("reader-sync_2"));
  CHECK(!validName("../bad"));
  CHECK(!validName("a.b"));
  CHECK(!validName("CROSSP~1"));
  CHECK(!validName(std::string(24, 'a').c_str()));
  CHECK(isNewerVersion("1.10.0", "1.9.9"));
  CHECK(!isNewerVersion("1.0.0", "1.0.0"));
  CHECK(!isNewerVersion("1.0.0-beta", "0.0.0"));
  CHECK(!isNewerVersion("4294967296.0.0", "1.0.0"));
  CHECK(isNewerVersion("1.0.0", ""));
  const auto digest = std::string(64, 'a'), changed = std::string(64, 'b');
  CHECK(PluginPermissions::approvalMatches(true, digest.c_str(), digest.c_str()));
  CHECK(!PluginPermissions::approvalMatches(false, digest.c_str(), digest.c_str()));
  CHECK(!PluginPermissions::approvalMatches(true, digest.c_str(), changed.c_str()));
  CHECK(!PluginPermissions::approvalMatches(true, "", digest.c_str()));
}
static void pathsAndRedirects() {
  using namespace protectedpaths;
  for (auto p :
       {"/.crosspoint/wifi.json", "/.CROSSPOINT/NUTSTORE.JSON.bak", "/books/../.crosspoint/opds.json",
        "/.crosspoint/weread/session", "/CROSSP~1/PLUGIN~1/A.JSON", "/.crosspoint/plugin-permissions/a.json.tmp",
        "/.crosspoint /plugin-permissions./a.json", "/plugins/demo/main.js", "/.PLUGINS/demo/plugin.js",
        "/books/.. /.crosspoint/plugin-permissions/a.json", "\\.crosspoint\\plugins\\demo\\config.json",
        "/plugins/demo/%2e%2e/main.js"})
    CHECK(isSensitivePath(p));
  CHECK(!isSensitivePath("/Books/title.epub"));
  CHECK(!isSensitivePath("/plugins-extra/book.epub"));
  CHECK(isPluginPath("/plugins/demo/main.js"));
  CHECK(isPluginPath("/books/title.epub.part"));
  for (auto p :
       {"/books/../.crosspoint/wifi.json", "/.crosspoint/weread/session", "/.crosspoint/plugin-permissions/a.json",
        "/books/BIGBOO~1.EPU", "/books/a /b", "/books//a", "/books/a.", "/books/a%00", "relative"})
    CHECK(!isPluginPath(p));
  CHECK(!isPluginPath(std::string("/books/a\0hidden", 15)));
  using namespace pluginhttp;
  CHECK(sameOrigin("https://EXAMPLE.com:443/a", "https://example.com/b"));
  CHECK(!sameOrigin("https://example.com", "https://example.com:8443"));
  CHECK(!validUrl("https://user:secret@example.com/a"));
  CHECK(!validUrl("https://example.com:65536/a"));
  CHECK(!validUrl("https://example.com/a\r\nX:1"));
  std::string url = "https://example.com/a";
  CHECK(!resolveRedirect(url, "http://example.com/b", url));
  CHECK(url == "https://example.com/a");
  CHECK(resolveRedirect(url, "/b", url));
  CHECK(url == "https://example.com/b");
  CHECK(resolveRedirect(url, "//other.example/c", url));
  CHECK(url == "https://other.example/c");
  CHECK(!sameOrigin("https://example.com", url));
  CHECK(validHeader("Authorization", "Bearer token"));
  CHECK(!validHeader("Host", "elsewhere"));
  CHECK(!validHeader("X-Safe", "ok\r\nAuthorization:secret"));
  CHECK(phaseTimeout(10, 0) == 60000);
  CHECK(phaseTimeout(10, 20010) == 2500);
  CHECK(phaseTimeout(10, 4010) == 1000);
  CHECK(phaseTimeout(20, 20) == 0);
  CHECK(phaseTimeout(UINT32_MAX - 10, 29) == 10);
  std::vector<std::pair<std::string, std::string>> headers(32, {"x", "a"});
  CHECK(responseHeadersWithinBudget(headers));
  headers.emplace_back("x", "a");
  CHECK(!responseHeadersWithinBudget(headers));
  headers.resize(1);
  headers[0].second = std::string(4096, 'a');
  CHECK(!responseHeadersWithinBudget(headers));
}
static void jobs() {
  PluginJobPool pool;
  pool.reset(100);
  CHECK(!pool.submit("demo", "sync", "{}", 0, false));
  CHECK(!pool.submit("../demo", "sync", "{}", 0, true));
  CHECK(!pool.submit("demo", "sync", std::string(192, 'a').c_str(), 0, true));
  uint32_t ids[6];
  for (size_t i = 0; i < 6; ++i) {
    auto* j = pool.submit("demo", "sync", "{}", i, true);
    CHECK(j);
    ids[i] = j->id;
  }
  CHECK(!pool.submit("demo", "sync", "{}", 7, true));
  CHECK(!pool.claim("demo", 10, false));
  CHECK(!pool.claim("other", 10, true));
  auto* j = pool.claim("demo", 10, true);
  CHECK(j && j->id == ids[0]);
  const auto stale = j->claim;
  CHECK(!pool.complete(j->id, stale, true, "{}", 11, false));
  CHECK(!pool.complete(j->id, stale + 1, true, "{}", 11, true));
  CHECK(!pool.complete(j->id, stale, true, std::string(192, 'x').c_str(), 11, true));
  CHECK(!pool.complete(j->id, stale, true, "{}", 10 + PluginJobPool::LEASE_MS, true));
  pool.expire(10 + PluginJobPool::LEASE_MS);
  CHECK(pool.find(ids[0])->state == PluginJobPool::Pending);
  // Other pending jobs retain their age; no pending/running slot is evicted.
  for (size_t i = 1; i < 6; ++i) {
    auto* claimed = pool.claim("demo", 700000, true);
    CHECK(claimed && claimed->id == ids[i]);
  }
  j = pool.claim("demo", 700000, true);
  CHECK(j && j->id == ids[0] && j->claim != stale);
  CHECK(!pool.complete(j->id, stale, true, "{}", 700001, true));
  CHECK(pool.complete(j->id, j->claim, true, "{}", 700001, true));
  CHECK(pool.complete(j->id, j->claim, true, "{}", 700002, true));
  CHECK(pool.submit("demo", "sync", "{}", 700003, true));
  pool.reset(1);
  CHECK(!pool.find(ids[0]));
  pool.submit("demo", "sync", "{}", UINT32_MAX - 100, true);
  j = pool.claim("demo", UINT32_MAX - 10, true);
  CHECK(pool.complete(j->id, j->claim, true, "{}", 20, true));
}
struct Delivery {
  std::vector<std::string> seen;
  int failAt = -1;
  bool cancelled = false;
};
static bool deliver(void* ctx, const char* text, size_t n) {
  auto& d = *static_cast<Delivery*>(ctx);
  d.seen.emplace_back(text, n);
  return int(d.seen.size()) != d.failAt;
}
static bool cancelled(void* ctx) { return static_cast<Delivery*>(ctx)->cancelled; }
static void eventsAndSession() {
  using namespace pluginevents;
  CHECK(recoverAppendLength("", 0) == 0);
  CHECK(recoverAppendLength("one\ntwo\n", 8) == 8);
  CHECK(recoverAppendLength("one\nbad", 7) == 4);
  CHECK(recoverAppendLength("all torn", 8) == 0);
  std::string full(4096, 'x');
  full.back() = '\n';
  CHECK(recoverAppendLength(full.data(), full.size()) == 4096);
  full.back() = 'x';
  CHECK(recoverAppendLength(full.data(), full.size()) == 0);
  full[1023] = '\n';
  CHECK(recoverAppendLength(full.data(), full.size()) == 1024);
  full += 'x';
  CHECK(recoverAppendLength(full.data(), full.size()) == SIZE_MAX);
  const std::string records = "{\"id\":\"fixed-1\"}\n{\"id\":\"fixed-2\"}\n{\"id\":\"fixed-3\"}\n";
  Delivery d;
  d.failAt = 2;
  auto prefix = drainPrefix(records.data(), records.size(), 4, deliver, &d);
  CHECK(prefix.attempts == 2 && prefix.stalled && prefix.bytes == records.find('\n') + 1);
  const auto tail = records.substr(prefix.bytes);
  d = {};
  auto retry = drainPrefix(tail.data(), tail.size(), 4, deliver, &d);
  CHECK(retry.bytes == tail.size());
  CHECK(d.seen.front() == "{\"id\":\"fixed-2\"}");
  d = {};
  d.cancelled = true;
  CHECK(drainPrefix(records.data(), records.size(), 4, deliver, &d, cancelled).attempts == 0);
  d = {};
  const std::string many = "1\n2\n3\n4\n5\n";
  CHECK(drainPrefix(many.data(), many.size(), 99, deliver, &d).attempts == 4);
  d = {};
  CHECK(drainPrefix("torn", 4, 4, deliver, &d).stalled);
  CHECK(d.seen.empty());
  const auto huge = std::string(512, 'x') + '\n';
  CHECK(drainPrefix(huge.data(), huge.size(), 4, deliver, &d).bytes == 0);
  CHECK(!sleepConnectAllowed(false, 100, true, true));
  CHECK(!sleepConnectAllowed(true, 19, true, true));
  CHECK(!sleepConnectAllowed(true, 20, false, true));
  CHECK(!sleepConnectAllowed(true, 20, true, false));
  CHECK(sleepConnectAllowed(true, 20, true, true));
  CHECK(withinDeadline(UINT32_MAX - 3, 20));
  CHECK(!withinDeadline(20, 20));
  ReaderSession s;
  CHECK(sizeof(s) <= 48);
  s.onRenderComplete(1000, 100, -5);
  s.noteTurn(true, true);
  s.onRenderComplete(2999, 101, 1000);
  CHECK(s.durationSeconds() == 0);
  s.noteTurn(true, false);
  s.onRenderComplete(10000, 109, 2000);
  CHECK(s.durationSeconds() == 0);
  s.noteTurn(false, true);
  s.onRenderComplete(20000, 119, 1000);
  CHECK(s.durationSeconds() == 0);
  s.noteTurn(true, true);
  s.onRenderComplete(30000, 129, 10000);
  CHECK(s.durationSeconds() == 10);
  CHECK(s.startTime() == 119 && s.endTime() == 129 && s.takeForFlush());
  CHECK(!s.takeForFlush());
  s.noteTurn(true, true);
  s.onRenderComplete(40000, 139, 0);
  CHECK(s.durationSeconds() == 10);
  s.reset();
  s.onRenderComplete(0, 100, 0);
  s.noteTurn(true, true);
  s.onRenderComplete(3600000, 3700, 20000);
  CHECK(s.durationSeconds() == 1800 && s.endProgressBp() == 10000);
  s.reset();
  s.onRenderComplete(0, 0, 0);
  s.noteTurn(true, true);
  s.onRenderComplete(20000, 100, 1000);
  CHECK(!s.takeForFlush());
  s.reset();
  s.onRenderComplete(UINT32_MAX - 999, 100, 0);
  s.noteTurn(true, true);
  s.onRenderComplete(9000, 110, 1000);
  CHECK(s.durationSeconds() == 10 && s.takeForFlush());
}
int main() {
  classificationAndPermission();
  pathsAndRedirects();
  jobs();
  eventsAndSession();
  std::cout << checks << " plugin policy/session checks passed\n";
}
