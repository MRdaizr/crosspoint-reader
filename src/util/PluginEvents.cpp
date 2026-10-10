#include "PluginEvents.h"

#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <esp_random.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PluginHttp.h"
#include "PluginHttpTemplates.h"
#include "PluginLocations.h"
#include "PluginPermissions.h"
#include "components/UITheme.h"
#include "network/ProtectedPaths.h"
#include "util/BookCacheUtils.h"
#include "util/TaskWatchdog.h"

namespace {

constexpr const char* EVENT_NAMES[] = {"reader.open", "reader.exit", "reader.session", "book.downloaded",
                                       "sleep.enter"};
static_assert(sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]) == static_cast<size_t>(pluginevents::Event::COUNT),
              "event name table out of sync");

constexpr const char* OUTBOX_NAME = "/events.jsonl";
// Preserve the oldest undelivered records; reject new appends once full.
constexpr size_t MAX_OUTBOX_BYTES = 4 * 1024;
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
constexpr size_t MAX_EVENT_LINE = 512;
// Event handler responses are acknowledgements, not content.
constexpr size_t MAX_EVENT_RESPONSE = 8 * 1024;

// Static subscription table: one slot per installed plugin that declares an
// "events" section. Rebuilt by refreshSubscriptions(); sized for a full
// plugin list screen, not a marketplace.
constexpr size_t MAX_EVENT_PLUGINS = 16;
struct Subscriber {
  char name[24] = {0};      // plugin folder name; "" = empty slot
  char dir[64] = {0};       // "<root>/<name>"
  uint8_t mask = 0;         // bit per Event
  uint8_t connectMask = 0;  // events whose handler declares "connect": true
};
Subscriber subscribers[MAX_EVENT_PLUGINS];
// A single mutex serializes the bounded outbox transaction across lifecycle and
// web tasks. Never called from ISR or the renderer. drain yields rather than
// competing with an emitter; parent owns network connection and cancellation.
SemaphoreHandle_t eventMutex() {
  static auto mutex = xSemaphoreCreateMutex();
  return mutex;
}
struct EventLock {
  bool locked = false;
  explicit EventLock(TickType_t wait = portMAX_DELAY) {
    auto m = eventMutex();
    locked = m && xSemaphoreTake(m, wait) == pdTRUE;
    if (!m) LOG_ERR("PEVT", "OOM: outbox mutex");
  }
  ~EventLock() {
    if (locked) xSemaphoreGive(eventMutex());
  }
};

uint8_t eventBit(const pluginevents::Event e) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(e)); }

int eventFromName(const char* name) {
  for (size_t i = 0; i < static_cast<size_t>(pluginevents::Event::COUNT); i++) {
    if (strcmp(EVENT_NAMES[i], name) == 0) return static_cast<int>(i);
  }
  return -1;
}

std::string outboxPath(const Subscriber& sub) { return std::string(sub.dir) + OUTBOX_NAME; }

struct CheckedOutboxClose {
  HalFile& file;
  const char* name;
  ~CheckedOutboxClose() {
    if (file.isOpen() && !file.close()) LOG_ERR("PEVT", "%s: outbox close failed", name);
  }
};
bool recoverOutboxTail(HalFile& file, const char* name) {
  const uint64_t length = file.fileSize64();
  if (length > MAX_OUTBOX_BYTES) {
    LOG_ERR("PEVT", "%s: oversized outbox; append skipped", name);
    return false;
  }
  if (!length) return true;
  if (!file.seek(static_cast<uint32_t>(length - 1))) return false;
  const int last = file.read();
  if (last < 0) return false;
  if (last == '\n') return true;
  // Allocate only for power-loss recovery, not for the normal append path.
  // One 256-byte checked buffer keeps the bounded backward scan off the stack.
  auto buffer = makeUniqueNoThrow<char[]>(256);
  if (!buffer) {
    LOG_ERR("PEVT", "OOM: torn-tail recovery");
    return false;
  }
  size_t cursor = length, keep = 0;
  while (cursor) {
    const size_t chunk = std::min<size_t>(cursor, 256);
    const size_t start = cursor - chunk;
    if (!file.seek(start) || file.read(buffer.get(), chunk) != static_cast<int>(chunk)) return false;
    const size_t boundary = pluginevents::recoverAppendLength(buffer.get(), chunk);
    if (boundary) {
      keep = start + boundary;
      break;
    }
    cursor = start;
    resetTaskWatchdogIfSubscribed();
  }
  if (!file.truncate(keep)) {
    LOG_ERR("PEVT", "%s: torn-tail truncate failed", name);
    return false;
  }
  LOG_DBG("PEVT", "%s: removed %u-byte torn tail", name, static_cast<unsigned>(length - keep));
  return true;
}

}  // namespace

namespace pluginevents {

void refreshSubscriptions() {
  EventLock lock;
  if (!lock.locked) return;
  for (auto& sub : subscribers) sub = Subscriber{};

  size_t slot = 0;
  for (const auto& entry : PluginLocations::scanPlugins()) {
    if (!entry.hasDevice) continue;
    if (slot >= MAX_EVENT_PLUGINS) {
      LOG_ERR("PEVT", "subscription table full; ignoring %s", entry.name.c_str());
      break;
    }
    // The fixed-size table cannot hold these: a truncated name or folder would
    // point emit() and drain() at a different outbox than the plugin's own.
    if (entry.name.size() >= sizeof(Subscriber::name) || entry.dir.size() >= sizeof(Subscriber::dir)) {
      LOG_ERR("PEVT", "plugin name/path too long for events; ignoring %s", entry.name.c_str());
      continue;
    }
    std::string raw;
    if (!Storage.readFileToString("PEVT", entry.dir + "/device.json", MAX_MANIFEST_SIZE, raw)) continue;
    // Filtered parse: only the events section, so a big manifest costs a few
    // hundred bytes here instead of a full document.
    JsonDocument filter;
    filter["events"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
    uint8_t mask = 0;
    uint8_t connectMask = 0;
    for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
      const int e = eventFromName(kv.key().c_str());
      if (e < 0) {
        LOG_DBG("PEVT", "%s: unknown event '%s' ignored", entry.name.c_str(), kv.key().c_str());
        continue;
      }
      mask |= static_cast<uint8_t>(1u << e);
      if (kv.value()["connect"] | false) connectMask |= static_cast<uint8_t>(1u << e);
    }
    // sleep.enter exists to act before the chip powers down (sleep image,
    // pre-sleep sync), so subscribing implies "connect": true; requiring the
    // flag would make the common case silently defer to the next session.
    if (mask & eventBit(Event::SleepEnter)) connectMask |= eventBit(Event::SleepEnter);
    if (mask == 0) continue;
    Subscriber& sub = subscribers[slot++];
    // Lengths checked above, so these copy whole strings.
    strncpy(sub.name, entry.name.c_str(), sizeof(sub.name) - 1);
    strncpy(sub.dir, entry.dir.c_str(), sizeof(sub.dir) - 1);
    sub.mask = mask;
    sub.connectMask = connectMask;
    LOG_DBG("PEVT", "%s subscribes mask=0x%02x", sub.name, sub.mask);
  }
}

bool anySubscriber(const Event e) {
  EventLock lock;
  if (!lock.locked) return false;
  if (e >= Event::COUNT) return false;
  for (const auto& sub : subscribers) {
    if (sub.name[0] != '\0' && (sub.mask & eventBit(e)) && PluginPermissions::allowed(sub.name)) return true;
  }
  return false;
}

uint8_t subscriptionMask(const char* plugin) {
  EventLock lock;
  if (!lock.locked || !plugin) return 0;
  for (const auto& sub : subscribers) {
    if (sub.name[0] != '\0' && strcmp(sub.name, plugin) == 0) return sub.mask;
  }
  return 0;
}

bool wantsConnectAny() {
  EventLock lock;
  if (!lock.locked) return false;
  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || sub.connectMask == 0 || !PluginPermissions::allowed(sub.name)) continue;
    std::string raw;
    if (!Storage.readFileToString("PEVT", outboxPath(sub), MAX_OUTBOX_BYTES, raw)) continue;
    for (size_t i = 0; i < static_cast<size_t>(Event::COUNT); i++) {
      if (!(sub.connectMask & eventBit(static_cast<Event>(i)))) continue;
      char eventField[48];
      snprintf(eventField, sizeof(eventField), "\"e\":\"%s\"", EVENT_NAMES[i]);
      if (raw.find(eventField) != std::string::npos) return true;
    }
  }
  return false;
}

bool shouldConnectForSleep(bool pluginSleepConnect, int batteryPercent, bool hasSavedNetwork) {
  return sleepConnectAllowed(pluginSleepConnect, batteryPercent, hasSavedNetwork,
                             pluginSleepConnect && batteryPercent >= 20 && hasSavedNetwork && wantsConnectAny());
}

void emit(const Event e, const Var* vars, const size_t varCount) {
  EventLock lock;
  if (!lock.locked || e >= Event::COUNT || varCount > 8 || (varCount && !vars)) return;
  if (!PluginPermissions::systemEnabled()) return;

  // One line: {"e":"reader.exit","id":"3fa9c21b-7","ts":1734212345,"vars":{"book":"...","percent":"74"}}
  JsonDocument doc;
  doc["e"] = EVENT_NAMES[static_cast<size_t>(e)];
  // Unique id for server-side dedupe of at-least-once delivery: a per-boot
  // nonce plus an in-session counter, unique across queued events and reboots
  // without an SD read-modify-write per event. ts alone repeats (1-second
  // resolution, and 0 whenever the clock was never set).
  static const uint32_t bootNonce = esp_random();
  static uint32_t seq = 0;
  char id[24];
  snprintf(id, sizeof(id), "%08lx-%lu", static_cast<unsigned long>(bootNonce), static_cast<unsigned long>(++seq));
  doc["id"] = id;
  // Best-effort unix time: 0 when the clock was never set (no RTC, no NTP yet).
  doc["ts"] = static_cast<long long>(halClock.nowUtc());
  JsonObject varsObj = doc["vars"].to<JsonObject>();
  for (size_t i = 0; i < varCount; i++) {
    if (!vars[i].key || !vars[i].value || strlen(vars[i].key) > 32 || strlen(vars[i].value) > 256) return;
    varsObj[vars[i].key] = vars[i].value;
  }
  std::string line;
  line.reserve(160);
  serializeJson(doc, line);
  line += '\n';
  if (line.size() > MAX_EVENT_LINE) {
    LOG_ERR("PEVT", "event line too large (%u); dropped", static_cast<unsigned>(line.size()));
    return;
  }

  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || !(sub.mask & eventBit(e)) || !PluginPermissions::allowed(sub.name)) continue;
    const std::string path = outboxPath(sub);
    HalFile file = Storage.open(path.c_str(), O_RDWR | O_CREAT | O_APPEND);
    if (!file || !file.isOpen()) {
      LOG_ERR("PEVT", "%s: outbox open failed", sub.name);
      continue;
    }
    CheckedOutboxClose close{file, sub.name};
    if (!recoverOutboxTail(file, sub.name)) {
      LOG_ERR("PEVT", "%s: outbox recovery failed; append skipped", sub.name);
      continue;
    }
    if (file.fileSize64() + line.size() > MAX_OUTBOX_BYTES) {
      LOG_ERR("PEVT", "%s: outbox full; keeping older undelivered events", sub.name);
      continue;
    }
    const uint64_t before = file.fileSize64();
    if (file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size()) != line.size()) {
      // A torn line would glue the next event onto it and the drain would drop
      // both as corrupt: cut back to the last complete line, losing only this one.
      LOG_ERR("PEVT", "%s: short outbox append; event dropped", sub.name);
      if (!file.truncate(before)) LOG_ERR("PEVT", "%s: outbox rollback failed", sub.name);
    }
    file.flush();
    if (!file.close()) LOG_ERR("PEVT", "%s: outbox close failed", sub.name);
  }
}

namespace {

// The manifest subset a drain needs: the handlers plus the token/config/auth
// vocabulary shared with the catalog browser.
struct DrainManifest {
  std::string tokenFile, tokenPath, configFile;
  std::string authType, authTokenPath;
  pluginhttp::RequestSpec authReq;
  struct Handler {
    int event = -1;
    pluginhttp::RequestSpec req;
    std::string toast;
    // "download" variant: the response streams to `dest` on SD (e.g. a fresh
    // sleep image) instead of being read as an acknowledgement.
    std::string dest;
    bool isDownload() const { return !dest.empty(); }
  };
  std::vector<Handler> handlers;
  bool hasPasswordGrant() const { return authType == "password" && !authReq.url.empty(); }
};

// False only when device.json cannot be read or parsed; a manifest that parses
// but declares no runnable handlers returns true with out.handlers empty.
bool loadDrainManifest(const Subscriber& sub, DrainManifest& out) {
  std::string raw;
  if (!Storage.readFileToString("PEVT", std::string(sub.dir) + "/device.json", MAX_MANIFEST_SIZE, raw)) return false;
  // Filtered parse: the drain runs at the heap-worst moments (sleep entry,
  // web session), so only the sections it reads are materialized.
  JsonDocument filter;
  filter["token"] = true;
  filter["config"] = true;
  filter["auth"] = true;
  filter["events"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;

  out.tokenFile = pluginhttp::inPluginDir(sub.dir, doc["token"]["file"] | "");
  out.tokenPath = doc["token"]["path"] | "token";
  out.configFile = pluginhttp::inPluginDir(sub.dir, doc["config"]["file"] | "");
  JsonVariantConst auth = doc["auth"];
  out.authType = auth["type"] | "device_code";
  pluginhttp::readRequest(auth["request"], "POST", out.authReq);
  out.authTokenPath = auth["token_path"] | "access_token";

  out.handlers.reserve(static_cast<size_t>(Event::COUNT));
  for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
    const int e = eventFromName(kv.key().c_str());
    if (e < 0) continue;
    DrainManifest::Handler h;
    h.event = e;
    JsonVariantConst dl = kv.value()["download"];
    if (dl["url"].as<const char*>()) {
      pluginhttp::readRequest(dl, "GET", h.req);
      h.dest = dl["dest"] | "";
      if (h.dest.empty()) continue;  // a download without a destination is meaningless
    } else {
      pluginhttp::readRequest(kv.value()["request"], "POST", h.req);
    }
    h.toast = kv.value()["toast"] | "";
    if (!h.req.url.empty()) out.handlers.push_back(std::move(h));
  }
  return true;
}

// {token}, {cfg.*}, {meta.*}, and {event.*} from the queued line's vars
// object. `config` and `meta` hold pre-built patterns ("{cfg.KEY}" /
// "{meta.KEY}") so the keys are not re-concatenated for every template.
std::string drainSubstituted(std::string tpl, const std::string& token, const pluginhttp::Headers& config,
                             const pluginhttp::Headers& meta, JsonVariantConst vars, const long long ts, const char* id,
                             pluginhttp::TemplateContext context = pluginhttp::TemplateContext::Text) {
  struct Values {
    const std::string* token;
    const pluginhttp::Headers* config;
    const pluginhttp::Headers* meta;
    JsonVariantConst vars;
    long long ts;
    const char* id;
  } values{&token, &config, &meta, vars, ts, id};
  std::string out;
  if (!pluginhttp::renderTemplate(
          tpl, context,
          [](void* ctx, std::string_view key, std::string& value) {
            const auto& v = *static_cast<Values*>(ctx);
            if (key == "token")
              value = *v.token;
            else if (key == "event.ts")
              value = std::to_string(v.ts);
            else if (key == "event.id")
              value = v.id;
            else if (key.starts_with("event."))
              value = pluginhttp::variantToString(v.vars[std::string(key.substr(6))]);
            else if (key.starts_with("cfg.") || key.starts_with("meta.")) {
              const bool configKey = key.starts_with("cfg.");
              const auto field = key.substr(configKey ? 4 : 5);
              for (const auto& kv : *(configKey ? v.config : v.meta))
                if (field == kv.first) {
                  value = kv.second;
                  return true;
                }
              value.clear();
            } else
              return false;
            return true;
          },
          &values, out)) {
    LOG_ERR("PEVT", "invalid event template/context");
    return context == pluginhttp::TemplateContext::Json || context == pluginhttp::TemplateContext::Form ||
                   context == pluginhttp::TemplateContext::Header
               ? std::string(1, '\1')
               : std::string{};
  }
  return out;
}

// Replays one queued line. True = delivered (drop the line); false = transport
// or auth failure (keep it for the next drain). A line with no matching
// handler counts as delivered so a manifest edit can't wedge the queue.
// `token` is shared across the drain: a 401-minted refresh persists to the
// remaining lines instead of re-minting per line.
bool deliverLine(const DrainManifest& mf, const std::string& lineText, std::string& token,
                 const pluginhttp::Headers& config, GfxRenderer* renderer, uint32_t deadlineMs,
                 const std::function<bool()>& abort) {
  JsonDocument doc;
  if (deserializeJson(doc, lineText) != DeserializationError::Ok) return true;  // corrupt line: drop
  const int e = eventFromName(doc["e"] | "");
  const DrainManifest::Handler* handler = nullptr;
  for (const auto& h : mf.handlers) {
    if (h.event == e) {
      handler = &h;
      break;
    }
  }
  if (!handler) return true;

  JsonVariantConst vars = doc["vars"];
  const long long ts = doc["ts"] | 0LL;
  // Lines queued by pre-id firmware substitute {event.id} as empty.
  const char* id = doc["id"] | "";

  // Book-scoped events expose the book's plugin sidecar ("<book>.meta.json",
  // flat fields written at download time) as {meta.*} variables, e.g. a
  // service book id for a sync handler.
  pluginhttp::Headers meta;
  const char* book = vars["book"] | (vars["path"] | "");
  if (book[0] != '\0') {
    pluginhttp::loadConfigFile(std::string(book) + ".meta.json", meta);
  }

  const auto run = [&](const std::string& tok) {
    pluginhttp::Headers headers;
    headers.reserve(handler->req.headers.size());
    for (const auto& h : handler->req.headers) {
      headers.emplace_back(
          h.first, drainSubstituted(h.second, tok, config, meta, vars, ts, id, pluginhttp::TemplateContext::Header));
    }
    if (handler->isDownload()) {
      const std::string dest =
          drainSubstituted(handler->dest, tok, config, meta, vars, ts, id, pluginhttp::TemplateContext::Path);
      // Substituted fields must not climb out of the tree or land on a
      // credential store (same guard as the catalog sidecar writer).
      if (!protectedpaths::isPluginPath(dest)) {
        LOG_ERR("PEVT", "unsafe download dest rejected: %s", dest.c_str());
        return 200;  // treat as delivered: retrying can never fix the manifest
      }
      // Generous for images (a 4-bit 800x480 BMP is ~192KB), still bounded.
      constexpr size_t MAX_EVENT_DOWNLOAD = 1024 * 1024;
      // Stream to a sibling temp and swap it in only on a clean 2xx, so a
      // 404/500 error body can never replace an existing dest (e.g. /sleep.bmp).
      const std::string tmp = dest + ".part";
      if (!protectedpaths::isPluginPath(tmp) || !protectedpaths::isPluginPath(dest + ".bak")) return 200;
      const int st = pluginhttp::requestToFile(
          nullptr,
          drainSubstituted(handler->req.url, tok, config, meta, vars, ts, id, pluginhttp::TemplateContext::Url),
          handler->req.method,
          drainSubstituted(handler->req.body, tok, config, meta, vars, ts, id,
                           pluginhttp::bodyTemplateContext(handler->req.body)),
          headers, tmp.c_str(), MAX_EVENT_DOWNLOAD, abort, deadlineMs);
      if (st >= 200 && st < 300) {
        // rename won't overwrite an existing file, so park the old dest as a
        // backup and restore it if the swap fails: a failed commit must not
        // lose both the old file and the fresh download. -1 (local failure,
        // same convention as pluginhttp) keeps the line queued for retry.
        if (!protectedpaths::isPluginPath(tmp) || !protectedpaths::isPluginPath(dest + ".bak") || abort() ||
            !Storage.replaceFile(tmp.c_str(), dest.c_str())) {
          Storage.remove(tmp.c_str());
          return -1;
        }
        invalidateBookCache(dest);
      } else {
        Storage.remove(tmp.c_str());
      }
      return st;
    }
    String response;
    return pluginhttp::request(
        nullptr, drainSubstituted(handler->req.url, tok, config, meta, vars, ts, id, pluginhttp::TemplateContext::Url),
        handler->req.method,
        drainSubstituted(handler->req.body, tok, config, meta, vars, ts, id,
                         pluginhttp::bodyTemplateContext(handler->req.body)),
        headers, response, MAX_EVENT_RESPONSE, nullptr, abort, deadlineMs);
  };

  int status = run(token);
  // A password-grant token expires; on 401/403 mint a fresh one and retry once.
  if ((status == 401 || status == 403) && mf.hasPasswordGrant()) {
    std::string minted;
    // Same template vocabulary as the delivery request (e.g. a {cfg.*} client
    // secret in an auth header).
    pluginhttp::Headers authHeaders;
    authHeaders.reserve(mf.authReq.headers.size());
    for (const auto& h : mf.authReq.headers) {
      authHeaders.emplace_back(
          h.first, drainSubstituted(h.second, token, config, meta, vars, ts, id, pluginhttp::TemplateContext::Header));
    }
    if (pluginhttp::mintPasswordToken(
            nullptr,
            drainSubstituted(mf.authReq.url, token, config, meta, vars, ts, id, pluginhttp::TemplateContext::Url),
            mf.authReq.method,
            drainSubstituted(mf.authReq.body, token, config, meta, vars, ts, id,
                             pluginhttp::bodyTemplateContext(mf.authReq.body)),
            authHeaders, mf.authTokenPath, minted, deadlineMs, abort)) {
      pluginhttp::saveTokenToFile(mf.tokenFile, mf.tokenPath, minted);
      token = minted;
      status = run(token);
    }
  }
  if (status < 200 || status >= 300) return false;

  if (renderer && !handler->toast.empty()) {
    GUI.drawPopup(*renderer, drainSubstituted(handler->toast, token, config, meta, vars, ts, id).c_str());
  }
  return true;
}

}  // namespace

void drain(GfxRenderer* renderer, const size_t maxEvents, uint32_t deadlineMs, bool (*cancel)(void*), void* context) {
  EventLock lock(0);
  if (!lock.locked) return;
  if (deadlineMs == 0) deadlineMs = millis() + SLEEP_BUDGET_MS;
  const auto abort = [&] {
    return !PluginPermissions::systemEnabled() || !withinDeadline(millis(), deadlineMs) || (cancel && cancel(context));
  };
  size_t budget = std::min<size_t>(maxEvents, MAX_DRAIN_EVENTS);
  for (const auto& sub : subscribers) {
    if (budget == 0 || abort()) break;
    if (sub.name[0] == '\0' || !PluginPermissions::allowed(sub.name)) continue;
    const std::string path = outboxPath(sub);
    if (!Storage.exists(path.c_str())) continue;

    std::string raw;
    if (!Storage.readFileToString("PEVT", path, MAX_OUTBOX_BYTES, raw)) continue;

    auto mf = makeUniqueNoThrow<DrainManifest>();  // 240-byte manifest stays off the ESP task stack
    if (!mf) {
      LOG_ERR("PEVT", "OOM: drain manifest");
      return;
    }
    // Unreadable or malformed right now (SD fault, a half-written manifest):
    // keep the queue for the next drain.
    if (!loadDrainManifest(sub, *mf)) continue;
    if (mf->handlers.empty()) {
      // Subscribed but no runnable handlers (JS-only consumer, or manifest
      // edited away): the queue would never advance, so clear it.
      Storage.remove(path.c_str());
      continue;
    }
    std::string token;
    pluginhttp::Headers config;
    pluginhttp::loadTokenFromFile(mf->tokenFile, mf->tokenPath, token);
    pluginhttp::loadConfigFile(mf->configFile, config);

    struct Delivery {
      const DrainManifest* manifest;
      std::string* token;
      const pluginhttp::Headers* config;
      GfxRenderer* renderer;
      uint32_t deadline;
      const decltype(abort)* stop;
      const char* plugin;
    } delivery{mf.get(), &token, &config, renderer, deadlineMs, &abort, sub.name};
    const auto consumed = drainPrefix(
        raw.data(), raw.size(), budget,
        [](void* ctx, const char* text, size_t length) {
          auto& d = *static_cast<Delivery*>(ctx);
          if (!PluginPermissions::allowed(d.plugin)) return false;
          return deliverLine(*d.manifest, std::string(text, length), *d.token, *d.config, d.renderer, d.deadline,
                             *d.stop);
        },
        &delivery, [](void* ctx) { return (*static_cast<Delivery*>(ctx)->stop)(); });
    const size_t pos = consumed.bytes;
    const bool stalled = consumed.stalled;
    budget -= consumed.attempts;

    if (pos >= raw.size() && !stalled) {
      Storage.remove(path.c_str());
    } else if (pos > 0) {
      // Rewrite the unprocessed tail so delivered events are not replayed.
      // writeFile keeps the old outbox if the rewrite fails: replaying delivered
      // events beats losing undelivered ones.
      const std::string tmp = path + ".tmp";
      if (!pluginhttp::writeStaged(tmp, raw.data() + pos, raw.size() - pos) ||
          !Storage.replaceFile(tmp.c_str(), path.c_str())) {
        LOG_ERR("PEVT", "%s: outbox rewrite failed", sub.name);
      }
    }
    LOG_DBG("PEVT", "%s: drained (stalled=%d)", sub.name, stalled);
  }
}

}  // namespace pluginevents
