#include "PluginCatalogActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "JsonListParser.h"
#include "MappedInputManager.h"
#include "PluginInfoActivity.h"
#include "XmlListParser.h"
#include "activities/reader/DictionaryDefinitionActivity.h"  // plain-text README viewer
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"
#include "network/ProtectedPaths.h"
#include "util/BookCacheUtils.h"
#include "util/PluginEvents.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#include "util/PluginPermissions.h"
#include "util/QrUtils.h"
#include "util/StringUtils.h"
#include "util/TaskWatchdog.h"

namespace fui = freeink::ui;

// Template/JSON/transport primitives shared with the plugin event drain.
using pluginhttp::resolvePath;
using pluginhttp::substituteAll;
using pluginhttp::urlEncodeQuery;
using pluginhttp::variantToString;

namespace {
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
// In-DRAM responses (auth, download-url hops) are small; the cap bounds a
// misbehaving server, not normal use.
constexpr size_t MAX_API_RESPONSE = 8 * 1024;
// Browse responses stream to this SD temp file instead of DRAM: one page of
// raw catalog JSON can run 60+ KB (BookFusion inlines heavy per-book
// metadata), and buffering that in a std::string aborts on low heap. Keep the
// response until leaving the catalog so downloads can release the parsed rows.
constexpr char BROWSE_TMP_PATH[] = "/.pcat_tmp.json";
constexpr size_t MAX_BROWSE_RESPONSE = 1024 * 1024;
constexpr int MAX_PAGE_SIZE = 16;

std::string md5Hex(const std::string& text) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  md5.calculate();
  return md5.toString().c_str();
}

// Returns `override` unless it's empty, in which case `fallback` applies.
const std::string& pick(const std::string& override, const std::string& fallback) {
  return override.empty() ? fallback : override;
}

// Plugin folder name from its "<root>/<name>/device.json" manifest path.
std::string pluginNameFromManifestPath(const std::string& manifestPath) {
  const size_t slash = manifestPath.rfind('/');
  if (slash == std::string::npos || slash == 0) return "";
  const size_t parent = manifestPath.rfind('/', slash - 1);
  if (parent == std::string::npos) return "";
  return manifestPath.substr(parent + 1, slash - parent - 1);
}

// book.downloaded plugin event, fired after a catalog download lands on SD.
void emitBookDownloaded(const std::string& manifestPath, const std::string& path, const std::string& title) {
  if (!pluginevents::anySubscriber(pluginevents::Event::BookDownloaded)) return;
  const std::string plugin = pluginNameFromManifestPath(manifestPath);
  const pluginevents::Var vars[] = {{"path", path.c_str()}, {"title", title.c_str()}, {"plugin", plugin.c_str()}};
  pluginevents::emit(pluginevents::Event::BookDownloaded, vars, 3);
}

// Streams the browse temp file through a catalog list parser (null on OOM).
// False when the reader cannot start or the document is malformed.
template <typename Parser>
bool streamBrowseFile(Parser* parser) {
  HalFile file;
  if (!parser || !Storage.openFileForRead("PCAT", BROWSE_TMP_PATH, file)) {
    LOG_ERR("PCAT", "Catalog list reader unavailable");
    return false;
  }
  if (parser->parse([](void* f, char* buf, size_t len) { return static_cast<HalFile*>(f)->read(buf, len); }, &file)) {
    return true;
  }
  LOG_ERR("PCAT", "Catalog list parse error");
  return false;
}

}  // namespace

namespace {
// Reads picker metadata, and classifies device manifests without making an
// events-only plugin look like an invalid catalog.
void readPluginMetadata(const std::string& path, PluginRef& ref, const bool classifyDevice = false) {
  std::string raw;
  if (!Storage.readFileToString("PCAT", path, MAX_MANIFEST_SIZE, raw)) return;
  JsonDocument filter;
  filter["title"] = true;
  filter["description"] = true;
  if (classifyDevice) {
    filter["browse"]["url"] = true;
    filter["events"] = true;
  }
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return;
  if (doc["title"].is<const char*>()) ref.title = doc["title"].as<const char*>();
  if (doc["description"].is<const char*>()) ref.description = doc["description"].as<const char*>();
  if (classifyDevice) {
    const char* browseUrl = doc["browse"]["url"] | "";
    const bool hasEvents = !doc["events"].as<JsonObjectConst>().isNull() && doc["events"].size() > 0;
    ref.deviceKind = PluginLocations::classifyDeviceManifest(browseUrl[0] != '\0', hasEvents);
  }
}
}  // namespace

std::vector<PluginRef> discoverPlugins() {
  const auto entries = PluginLocations::scanPlugins();
  std::vector<PluginRef> plugins;
  plugins.reserve(entries.size());
  for (const auto& e : entries) {
    PluginRef ref;
    ref.name = e.name;
    ref.title = e.name;
    // Browser-only plugins (no device.json) stay listed so an install is
    // visibly installed, but carry no manifest to open (empty manifestPath).
    if (e.hasDevice) ref.manifestPath = e.dir + "/device.json";
    const std::string readmePath = e.dir + "/README.md";
    if (Storage.exists(readmePath.c_str())) ref.readmePath = readmePath;
    // manifest.json first, then device.json overrides (on-device authority).
    if (e.hasManifest) readPluginMetadata(e.dir + "/manifest.json", ref);
    if (e.hasDevice) readPluginMetadata(ref.manifestPath, ref, true);
    plugins.push_back(std::move(ref));
  }
  return plugins;
}

bool anyPluginInstalled() {
  // manifest.json alone means web-card metadata only; plugin.js or device.json
  // is what makes an installed plugin worth the home screen's library slot.
  for (const auto& e : PluginLocations::scanPlugins()) {
    if (e.hasPluginJs || e.hasMainJs || e.hasDevice) return true;
  }
  return false;
}

bool PluginCatalogActivity::loadManifest() {
  std::string raw;
  if (!Storage.readFileToString("PCAT", manifestPath, MAX_MANIFEST_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;

  const std::string pluginDir = manifestPath.substr(0, manifestPath.rfind('/'));
  manifest.tokenFile = pluginhttp::inPluginDir(pluginDir, doc["token"]["file"] | "");
  manifest.tokenPath = doc["token"]["path"] | "token";
  manifest.configFile = pluginhttp::inPluginDir(pluginDir, doc["config"]["file"] | "");

  JsonVariantConst browse = doc["browse"];
  manifest.browseFormat = browse["format"] | "json";
  pluginhttp::readRequest(browse, "GET", manifest.browseReq);
  manifest.itemsPath = browse["items"] | "";
  manifest.titlePath = browse["fields"]["title"] | (manifest.isXmlList() ? "" : "title");
  manifest.authorPath = browse["fields"]["author"] | "";
  manifest.idPath = browse["fields"]["id"] | "";
  manifest.urlPath = browse["fields"]["url"] | "";
  manifest.versionPath = browse["fields"]["version"] | "";
  manifest.pageSize = browse["page_size"] | 8;
  // Documented bounds: each row costs an Item (strings) and a screen slot.
  manifest.pageSize = std::clamp(manifest.pageSize, 1, MAX_PAGE_SIZE);
  manifest.browseLists.reserve(8);
  for (JsonVariantConst l : browse["lists"].as<JsonArrayConst>()) {
    if (manifest.browseLists.size() >= 8) break;
    Manifest::BrowseList entry;
    entry.title = l["title"] | "";
    entry.url = l["url"] | "";
    entry.body = l["body"] | "";
    if (!entry.title.empty()) manifest.browseLists.push_back(std::move(entry));
  }
  manifest.searchUrl = browse["search"]["url"] | "";
  manifest.searchBody = browse["search"]["body"] | "";
  manifest.xmlItem = browse["item"] | "";
  manifest.xmlContainer = browse["container_element"] | "";
  manifest.xmlSkipSelf = browse["skip_self"] | false;
  manifest.xmlResolveUrls = browse["resolve_urls"] | false;
  manifest.xmlExtensions.reserve(8);
  for (JsonVariantConst ext : browse["extensions"].as<JsonArrayConst>()) {
    if (manifest.xmlExtensions.size() >= 8) break;
    if (ext.is<const char*>()) manifest.xmlExtensions.emplace_back(ext.as<const char*>());
  }

  JsonVariantConst dl = doc["download"];
  pluginhttp::readRequest(dl, "GET", manifest.downloadReq);
  manifest.dlUrlPath = dl["url_path"] | "";
  manifest.dlUser = dl["username"] | "";
  manifest.dlPass = dl["password"] | "";
  manifest.destDir = pluginhttp::inPluginDir(pluginDir, dl["dest_dir"] | "");
  if (!manifest.destDir.empty() && !protectedpaths::isPluginPath(manifest.destDir)) {
    LOG_ERR("PCAT", "dest_dir outside plugin space: %s", manifest.destDir.c_str());
    return false;
  }
  manifest.filenameTpl = dl["filename"] | "{title}.epub";
  // Multi-file bundle install (generic): base URL + a files array per item.
  manifest.bundleBasePath = dl["bundle"]["base"] | "";
  manifest.bundleFilesPath = dl["bundle"]["files"] | "";
  manifest.bundleSubdir = dl["bundle"]["subdir"] | "{id}";
  // XML-list items already carry the file URL; default the template to it.
  if (manifest.isXmlList() && manifest.downloadReq.url.empty()) manifest.downloadReq.url = "{url}";
  manifest.sidecarPath = dl["sidecar"]["path"] | "";
  manifest.sidecarBody = dl["sidecar"]["body"] | "";

  JsonVariantConst auth = doc["auth"];
  manifest.authType = auth["type"] | "device_code";
  pluginhttp::readRequest(auth["request"], "POST", manifest.authReq);
  pluginhttp::readRequest(auth["poll"], "POST", manifest.pollReq);
  manifest.authCodePath = auth["code_path"] | "user_code";
  manifest.authVerifyPath = auth["verify_url_path"] | "verification_uri";
  manifest.authDeviceCodePath = auth["device_code_path"] | "device_code";
  manifest.authIntervalPath = auth["interval_path"] | "interval";
  manifest.authExpiresPath = auth["expires_path"] | "expires_in";
  manifest.authTokenPath = auth["token_path"] | "access_token";
  manifest.authErrorPath = auth["error_path"] | "error";

  return !manifest.browseReq.url.empty();
}

bool PluginCatalogActivity::saveToken(const std::string& value) {
  return pluginhttp::saveTokenToFile(manifest.tokenFile, manifest.tokenPath, value);
}

bool PluginCatalogActivity::loadToken() {
  return pluginhttp::loadTokenFromFile(manifest.tokenFile, manifest.tokenPath, token);
}

void PluginCatalogActivity::loadConfig() { pluginhttp::loadConfigFile(manifest.configFile, config); }

pluginhttp::Headers PluginCatalogActivity::substitutedHeaders(const pluginhttp::Headers& headers,
                                                              const Item* item) const {
  pluginhttp::Headers out;
  out.reserve(headers.size());
  for (const auto& h : headers)
    out.emplace_back(h.first, substituted(h.second, item, pluginhttp::TemplateContext::Header));
  return out;
}

pluginhttp::RequestSpec PluginCatalogActivity::substitutedRequest(const pluginhttp::RequestSpec& req,
                                                                  const Item* item) const {
  return {substituted(req.url, item, pluginhttp::TemplateContext::Url), req.method,
          substituted(req.body, item, pluginhttp::bodyTemplateContext(req.body)),
          substitutedHeaders(req.headers, item)};
}

std::string PluginCatalogActivity::substituted(std::string tpl, const Item* item, pluginhttp::TemplateContext context,
                                               const std::string* md5, const std::string* dest) const {
  struct Values {
    const PluginCatalogActivity* self;
    const Item* item;
    const std::string* md5;
    const std::string* dest;
  } values{this, item, md5, dest};
  std::string out;
  const bool ok = pluginhttp::renderTemplate(
      tpl, context,
      [](void* opaque, std::string_view key, std::string& value) {
        const auto& v = *static_cast<Values*>(opaque);
        const auto& self = *v.self;
        if (key == "token")
          value = self.token;
        else if (key == "page")
          value = std::to_string(self.page);
        else if (key == "limit")
          value = std::to_string(self.manifest.pageSize + 1);
        else if (key == "query" || key == "query_raw")
          value = self.searchQuery;
        else if (key == "device_code")
          value = self.authDeviceCode;
        else if (key == "md5" && v.md5)
          value = *v.md5;
        else if (key == "dest" && v.dest)
          value = *v.dest;
        else if (key.starts_with("cfg.")) {
          for (const auto& kv : self.config)
            if (key.substr(4) == kv.first) {
              value = kv.second;
              return true;
            }
          value.clear();
        } else if (v.item && key == "id")
          value = v.item->id;
        else if (v.item && key == "title")
          value = v.item->title;
        else if (v.item && key == "author")
          value = v.item->author;
        else if (v.item && key == "url")
          value = v.item->url;
        else
          return false;
        return true;
      },
      &values, out);
  if (!ok) {
    LOG_ERR("PCAT", "invalid template/context");
    // A control sentinel makes failed bodies/headers fail transport validation.
    return context == pluginhttp::TemplateContext::Json || context == pluginhttp::TemplateContext::Form ||
                   context == pluginhttp::TemplateContext::Header
               ? std::string(1, '\1')
               : std::string{};
  }
  return out;
}

std::string PluginCatalogActivity::downloadDir() const {
  for (const auto& kv : config) {
    if (kv.first != "dest_dir" || kv.second.empty()) continue;
    std::string dir = kv.second.front() == '/' ? kv.second : "/" + kv.second;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (dir == "/") return "";  // SD root
    if (protectedpaths::isPluginPath(dir)) return dir;
    LOG_ERR("PCAT", "config dest_dir rejected: %s", kv.second.c_str());
  }
  return manifest.destDir;
}

PluginCatalogActivity::PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const bool showOpds, const bool rootMode)
    : CatalogActivity("PluginCatalog", renderer, mappedInput), showOpds(showOpds), rootMode(rootMode) {}

PluginCatalogActivity::~PluginCatalogActivity() = default;

int PluginCatalogActivity::apiRequest(const pluginhttp::RequestSpec& req, String& out) {
  if (!PluginPermissions::allowed(pluginNameFromManifestPath(manifestPath).c_str())) return -1;
  return pluginhttp::request(session.get(), req.url, req.method, req.body, req.headers, out, MAX_API_RESPONSE, nullptr,
                             [this] { return cancelRequested.load() || !PluginPermissions::systemEnabled(); });
}

void PluginCatalogActivity::onEnter() {
  CatalogActivity::onEnter();
  enterPluginPicker();
}

void PluginCatalogActivity::enterPluginPicker() {
  Storage.remove(BROWSE_TMP_PATH);
  installedPlugins = discoverPlugins();
  // Discovery just re-read the plugin folders; keep the event subscription
  // table in step so a plugin installed since boot starts receiving events
  // (and a removed one stops) without a restart.
  pluginevents::refreshSubscriptions();
  // Disclose which device events each plugin receives (and so what reading
  // activity it can send off the device). The list leads the subtitle so the
  // two-line wrap never cuts it.
  static constexpr StrId EVENT_LABELS[] = {StrId::STR_EVENT_BOOK_OPEN, StrId::STR_EVENT_BOOK_CLOSE,
                                           StrId::STR_EVENT_READING_SESSION, StrId::STR_EVENT_DOWNLOAD,
                                           StrId::STR_EVENT_SLEEP};
  static_assert(sizeof(EVENT_LABELS) / sizeof(EVENT_LABELS[0]) == static_cast<size_t>(pluginevents::Event::COUNT));
  for (auto& plugin : installedPlugins) {
    const uint8_t mask = pluginevents::subscriptionMask(plugin.name.c_str());
    if (mask == 0) continue;
    std::string events;
    for (size_t i = 0; i < std::size(EVENT_LABELS); i++) {
      if (!(mask & (1u << i))) continue;
      if (!events.empty()) events += ", ";
      events += I18N.get(EVENT_LABELS[i]);
    }
    char line[192];
    snprintf(line, sizeof(line), tr(STR_PLUGIN_RECEIVES_EVENTS), events.c_str());
    plugin.description = plugin.description.empty() ? line : std::string(line) + ". " + plugin.description;
  }

  manifestPath.clear();
  manifest = Manifest{};
  catalogTitle = tr(STR_PLUGINS);
  token.clear();
  config.clear();
  resetBrowse();
  browseHistory.clear();
  browseCurrentUrl.clear();
  errorMessage.clear();
  session.reset();  // no TLS while only picking
  state = State::PLUGIN_PICKER;
  if (pickerReturnRow > 0 && pickerReturnRow < rowCount()) moveSelectionTo(pickerReturnRow);
  requestUpdate();
}

void PluginCatalogActivity::enterCatalog() {
  if (!PluginPermissions::allowed(pluginNameFromManifestPath(manifestPath).c_str())) {
    fail(StrId::STR_PLUGIN_DISABLED);
    return;
  }
  state = State::CHECK_WIFI;
  resetBrowse();
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  session.reset(new (std::nothrow) freeink::SecureHttpClient());
  if (session)
    session->setReuse(true);
  else
    LOG_ERR("PCAT", "OOM: browse client; per-request fallback");

  if (!loadManifest()) {
    fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
    return;
  }
  requestUpdate();
  checkAndConnectWifi();
}

// Leaving the open catalog (Back at its root, or Back out of an error /
// sign-in screen): return to the plugin picker. Wi-Fi stays up so the next
// pick connects instantly; onExit tears it down when the activity ends.
void PluginCatalogActivity::exitCatalog() { enterPluginPicker(); }

void PluginCatalogActivity::resetBrowse() {
  items.clear();
  page = 1;
  hasMore = false;
  currentList = -1;
  searchActive = false;
  searchQuery.clear();
  releaseRows();
  nav.reset();
}

bool PluginCatalogActivity::wantsListPicker() const {
  return !manifest.browseLists.empty() && !manifest.isXmlList() && currentList < 0;
}

void PluginCatalogActivity::onExit() {
  stopTransfer();
  closeRouting();
  items.clear();
  session.reset();  // drop browse TLS before Wi-Fi teardown
  Storage.remove(BROWSE_TMP_PATH);
  CatalogActivity::onExit();
}

void PluginCatalogActivity::startBrowse() {
  // Browse lists apply to JSON catalogs; XML lists navigate by folder instead.
  if (wantsListPicker()) {
    // Same auth gate as fetchPage: without it a signed-out user is shown the
    // list picker and only hits the sign-in screen after picking a list.
    // loadToken() returns true for token-less catalogs, which skip the gate.
    loadConfig();
    if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
      if (manifest.hasDeviceCode()) {
        beginAuth();  // straight to the QR/code sign-in, no interstitial
      } else {
        state = State::NO_TOKEN;
        requestUpdate();
      }
      return;
    }
    items.clear();
    page = 1;
    hasMore = false;
    releaseRows();
    nav.reset();
    state = State::LIST_PICKER;
    requestUpdate();
    return;
  }
  beginLoading();
  fetchPage(1);
}

void PluginCatalogActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    requestUpdate();
    return;
  }
  searchQuery = query;
  searchActive = true;
  currentList = -1;  // search spans the whole catalog, not a single list
  releaseRows();
  nav.reset();
  beginLoading();
  fetchPage(1);
}

bool PluginCatalogActivity::refreshCredentialToken() {
  loadConfig();
  std::string minted;
  if (!pluginhttp::mintPasswordToken(
          session.get(), substituted(manifest.authReq.url, nullptr, pluginhttp::TemplateContext::Url),
          manifest.authReq.method,
          substituted(manifest.authReq.body, nullptr, pluginhttp::bodyTemplateContext(manifest.authReq.body)),
          substitutedHeaders(manifest.authReq.headers, nullptr), manifest.authTokenPath, minted)) {
    return false;
  }
  if (!saveToken(minted)) return false;
  token = minted;  // usable immediately, without re-reading the file
  return true;
}

bool PluginCatalogActivity::fetchBrowseResponse() {
  if (!PluginPermissions::allowed(pluginNameFromManifestPath(manifestPath).c_str())) {
    fail(StrId::STR_PLUGIN_DISABLED);
    return false;
  }
  loadConfig();
  if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
    state = State::NO_TOKEN;
    requestUpdate();
    return false;
  }
  if (manifest.isXmlList()) {
    if (manifest.xmlItem.empty()) {
      fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
      return false;
    }
    if (browseCurrentUrl.empty())
      browseCurrentUrl = substituted(manifest.browseReq.url, nullptr, pluginhttp::TemplateContext::Url);
  }
  const auto run = [&] {
    const pluginhttp::RequestSpec req = {
        substituted(manifest.isXmlList() ? browseCurrentUrl : activeBrowseUrl(), nullptr,
                    pluginhttp::TemplateContext::Url),
        manifest.browseReq.method,
        substituted(
            manifest.isXmlList() ? manifest.browseReq.body : activeBrowseBody(), nullptr,
            pluginhttp::bodyTemplateContext(manifest.isXmlList() ? manifest.browseReq.body : activeBrowseBody())),
        substitutedHeaders(manifest.browseReq.headers, nullptr)};
    return pluginhttp::requestToFile(session.get(), req.url, req.method, req.body, req.headers, BROWSE_TMP_PATH,
                                     MAX_BROWSE_RESPONSE);
  };
  int status = run();
  // Rebuild templates with the refreshed token; retry only once.
  if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) status = run();
  if (status >= 200 && status < 300) return true;  // includes WebDAV's 207 Multi-Status
  Storage.remove(BROWSE_TMP_PATH);
  if (status == 401 || status == 403) {
    state = State::NO_TOKEN;
    requestUpdate();
  } else {
    fail(StrId::STR_FETCH_FEED_FAILED);
  }
  return false;
}

bool PluginCatalogActivity::parseXmlList() {
  releaseRows();
  items.clear();
  auto append = [&](XmlListParser::RawItem& row) {
    Item item;
    item.isDir = row.isDir;
    item.url = std::move(row.field[XmlListParser::F_URL]);
    item.title = std::move(row.field[XmlListParser::F_TITLE]);
    item.author = std::move(row.field[XmlListParser::F_AUTHOR]);
    item.id = std::move(row.field[XmlListParser::F_ID]);
    if (items.size() == items.capacity()) {
      items.reserve(std::min(XmlListParser::MAX_ITEMS, std::max<size_t>(manifest.pageSize, items.capacity() * 2)));
    }
    items.push_back(std::move(item));
  };
  const std::string* const selectors[] = {&manifest.urlPath, &manifest.titlePath, &manifest.authorPath,
                                          &manifest.idPath};
  auto parser = makeUniqueNoThrow<XmlListParser>(
      manifest.xmlItem, manifest.xmlContainer, selectors,
      [](void* context, XmlListParser::RawItem& row) { (*static_cast<decltype(append)*>(context))(row); }, &append);
  if (parser) {
    XmlListParser::UrlOptions urls;
    urls.requestUrl = browseCurrentUrl;
    urls.skipSelf = manifest.xmlSkipSelf;
    urls.resolveUrls = manifest.xmlResolveUrls;
    urls.extensions = manifest.xmlExtensions;
    parser->setUrlOptions(std::move(urls));
  }
  if (!streamBrowseFile(parser.get())) {
    fail(StrId::STR_PARSE_FEED_FAILED);
    return false;
  }

  // Folders first, then files, each alphabetical — matches how file managers list.
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
  });
  hasMore = false;
  return true;
}

bool PluginCatalogActivity::prevRowVisible() const {
  return state == State::BROWSING && !manifest.isXmlList() && page > 1;
}

bool PluginCatalogActivity::nextRowVisible() const {
  return state == State::BROWSING && !manifest.isXmlList() && hasMore;
}

int PluginCatalogActivity::rowCount() const {
  if (state == State::PLUGIN_PICKER) return static_cast<int>(installedPlugins.size()) + (showOpds ? 1 : 0);
  if (state == State::LIST_PICKER) return static_cast<int>(manifest.browseLists.size());
  if (state != State::BROWSING) return 0;
  return static_cast<int>(items.size()) + (prevRowVisible() ? 1 : 0) + (nextRowVisible() ? 1 : 0);
}

const std::string& PluginCatalogActivity::activeBrowseUrl() const {
  // Search overrides the list view, reusing the browse url when unspecified.
  if (searchActive) return pick(manifest.searchUrl, manifest.browseReq.url);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseReq.url;
  return pick(manifest.browseLists[currentList].url, manifest.browseReq.url);
}

const std::string& PluginCatalogActivity::activeBrowseBody() const {
  if (searchActive) return pick(manifest.searchBody, manifest.browseReq.body);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseReq.body;
  return pick(manifest.browseLists[currentList].body, manifest.browseReq.body);
}

void PluginCatalogActivity::fetchPage(const int newPage) {
  if (!manifest.isXmlList()) page = newPage;
  if (!fetchBrowseResponse() || !parseBrowseResponse()) return;
  nav.reset();
  state = State::BROWSING;
  requestUpdate();
}

bool PluginCatalogActivity::parseBrowseResponse() {
  if (manifest.isXmlList()) return parseXmlList();

  releaseRows();
  items.clear();
  items.reserve(manifest.pageSize + 1);
  // One extra row reveals whether a next page exists.
  auto append = [&](JsonListParser::Row& row) {
    if (row.field[JsonListParser::F_TITLE].empty() || static_cast<int>(items.size()) > manifest.pageSize) return;
    Item item;
    item.title = std::move(row.field[JsonListParser::F_TITLE]);
    item.author = std::move(row.field[JsonListParser::F_AUTHOR]);
    item.id = std::move(row.field[JsonListParser::F_ID]);
    item.url = std::move(row.field[JsonListParser::F_URL]);
    item.version = std::move(row.field[JsonListParser::F_VERSION]);
    item.base = std::move(row.field[JsonListParser::F_BASE]);
    item.files = std::move(row.files);
    items.push_back(std::move(item));
  };
  // Streamed straight from the SD temp file: neither the raw response nor a
  // parsed document occupies DRAM, only the rows kept.
  const std::string* const fields[] = {&manifest.titlePath, &manifest.authorPath,  &manifest.idPath,
                                       &manifest.urlPath,   &manifest.versionPath, &manifest.bundleBasePath};
  auto parser = makeUniqueNoThrow<JsonListParser>(
      manifest.itemsPath, fields, manifest.bundleFilesPath,
      [](void* context, JsonListParser::Row& row) { (*static_cast<decltype(append)*>(context))(row); }, &append);
  if (!streamBrowseFile(parser.get())) {
    fail(StrId::STR_PARSE_FEED_FAILED);
    return false;
  }
  hasMore = static_cast<int>(items.size()) > manifest.pageSize;
  if (hasMore) items.resize(manifest.pageSize);
  computeInstallStatus();
  return true;
}

// Badge each item by comparing its catalog version to the installed copy's
// manifest (located by folder id across the plugin roots). Only a newer
// catalog version is an update, so an older catalog never offers a downgrade.
// Runs once per page.
void PluginCatalogActivity::computeInstallStatus() {
  if (!manifest.tracksInstalls()) return;
  JsonDocument filter;
  filter["version"] = true;
  for (auto& item : items) {
    item.status.clear();
    if (item.id.empty()) continue;
    // Locate the install across every plugin root (not just one), matching
    // the discovery the rest of the firmware uses.
    const std::string dir = PluginLocations::findPluginDir(item.id.c_str());
    std::string raw;
    if (dir.empty() || !Storage.readFileToString("PCAT", dir + "/manifest.json", MAX_MANIFEST_SIZE, raw)) {
      // Not installed: show the available version so the row is not blank.
      if (!item.version.empty()) item.status = "v" + item.version;
      continue;
    }
    JsonDocument doc;
    std::string installed;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
      installed = doc["version"] | "";
    }
    item.status =
        PluginLocations::isNewerVersion(item.version, installed) ? tr(STR_UPDATE_AVAILABLE) : tr(STR_INSTALLED);
  }
}

void PluginCatalogActivity::beginAuth() {
  beginLoading();

  String response;
  const int status = apiRequest(substitutedRequest(manifest.authReq), response);
  JsonDocument doc;
  if (status < 200 || status >= 300 || deserializeJson(doc, response) != DeserializationError::Ok) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  authUserCode = variantToString(resolvePath(root, manifest.authCodePath));
  authVerifyUrl = variantToString(resolvePath(root, manifest.authVerifyPath));
  authDeviceCode = variantToString(resolvePath(root, manifest.authDeviceCodePath));
  const long interval = resolvePath(root, manifest.authIntervalPath) | 5L;
  const long expires = resolvePath(root, manifest.authExpiresPath) | 900L;
  if (authUserCode.empty() || authDeviceCode.empty()) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  authIntervalMs = std::clamp<long>(interval, 3, 60) * 1000UL;
  authDeadlineMs = millis() + std::clamp<long>(expires, 60, 1800) * 1000UL;
  authNextPollMs = millis() + authIntervalMs;
  state = State::AUTH;
  requestUpdate();
}

void PluginCatalogActivity::pollAuth() {
  // Checked before polling so an expired code fails even while every poll
  // hits a transport error (offline).
  if (static_cast<long>(millis() - authDeadlineMs) >= 0) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  authNextPollMs = millis() + authIntervalMs;

  auto req = substitutedRequest(manifest.pollReq);

  String response;
  const int status = apiRequest(req, response);
  if (status < 0) return;  // transient transport failure: keep polling

  JsonDocument doc;
  if (deserializeJson(doc, response) == DeserializationError::Ok) {
    const JsonVariantConst root = doc.as<JsonVariantConst>();
    const std::string newToken = variantToString(resolvePath(root, manifest.authTokenPath));
    if (!newToken.empty()) {
      if (!saveToken(newToken)) {
        fail(StrId::STR_PLUGIN_AUTH_FAILED);
        return;
      }
      startBrowse();
      return;
    }
    const std::string code = variantToString(resolvePath(root, manifest.authErrorPath));
    if (code == "slow_down") {
      authIntervalMs += 5000;
    } else if (code == "expired_token" || code == "access_denied") {
      fail(StrId::STR_PLUGIN_AUTH_FAILED);
      return;
    }
    // authorization_pending (or anything unrecognized): keep polling
  }
}

void PluginCatalogActivity::downloadItem(const int itemIndex) {
  if (!PluginPermissions::allowed(pluginNameFromManifestPath(manifestPath).c_str())) {
    fail(StrId::STR_PLUGIN_DISABLED);
    return;
  }
  // Own only the selected item's metadata while TLS needs the catalog's heap.
  {
    RenderLock lock;
    transferItem = std::move(items[itemIndex]);
    beginDownload(transferItem.title);
    releaseRows();
    std::vector<fui::ListItem>().swap(rowItems);
    std::vector<Item>().swap(items);
  }
  requestUpdateAndWait();
  transferDone = false;
  // One activity-owned network task keeps main-loop GPIO polling alive. Its
  // bounded 6KB stack contains no feed buffers; onExit cancels and joins it.
  if (xTaskCreate(&PluginCatalogActivity::transferTrampoline, "PluginDownload", 6144, this, 1, &transferTask) !=
      pdPASS) {
    LOG_ERR("PCAT", "OOM: transfer task");
    transferItem = {};
    fail(StrId::STR_MEMORY_ERROR);
  }
}

void PluginCatalogActivity::transferTrampoline(void* context) {
  auto* self = static_cast<PluginCatalogActivity*>(context);
  self->transferResult = self->manifest.isBundle() && !self->transferItem.files.empty()
                             ? self->downloadBundle(self->transferItem)
                             : self->downloadBook(self->transferItem);
  self->session.reset();
  self->transferDone.store(true);
  // Keep the handle valid until the main task deletes it after observing done.
  vTaskSuspend(nullptr);
}
void PluginCatalogActivity::stopTransfer() {
  if (!transferTask) return;
  cancelRequested = true;
  while (!transferDone.load()) {
    resetTaskWatchdogIfSubscribed();
    vTaskDelay(1);
  }
  vTaskDelete(transferTask);
  transferTask = nullptr;
  transferItem = {};
}

void PluginCatalogActivity::downloadFinished(const bool cancelled) {
  state = cancelled ? State::BROWSING : State::DONE;
  requestUpdate();
}

HttpDownloader::DownloadError PluginCatalogActivity::downloadBundle(const Item& item) {
  session.reset();
  const std::string subdir = substituted(manifest.bundleSubdir, &item);
  // Reject path traversal in the subdir (a hostile catalog could escape).
  if (subdir.empty() || subdir.find("..") != std::string::npos || subdir.front() == '/') {
    return HttpDownloader::FILE_ERROR;
  }
  std::string dir = downloadDir();
  if (!dir.empty() && dir.back() == '/') dir.pop_back();
  dir += '/';
  dir += subdir;
  if (!Storage.exists(dir.c_str()) && !Storage.mkdir(dir.c_str())) {
    LOG_ERR("PCAT", "bundle mkdir failed: %s", dir.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  std::string base = item.base;
  if (!base.empty() && base.back() != '/') base += '/';
  const size_t total = item.files.size();
  if (total > 32) return HttpDownloader::FILE_ERROR;
  // Every file downloads to <dest>.new first and replaces <dest> only once the
  // whole bundle has arrived, so a failed or cancelled update leaves the
  // installed plugin/theme as it was (a half-installed folder would show up
  // broken in the next discovery scan).
  std::vector<std::string> staged;  // final destinations with a complete .new
  staged.reserve(total);
  bool complete = false;
  const auto cleanup = [&] {
    if (complete) return;
    for (const auto& path : staged) Storage.remove((path + ".new").c_str());
    Storage.rmdir(dir.c_str());  // only succeeds when the folder is empty (a fresh install)
  };
  struct Rollback {
    const decltype(cleanup)& fn;
    ~Rollback() { fn(); }
  } rollback{cleanup};
  for (size_t i = 0; i < total; i++) {
    std::string rel = item.files[i];
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    const std::string dest = dir + "/" + rel;
    if (rel.empty() || !protectedpaths::isPluginPath(dest)) {
      // A manifest listing traversal or credential-store entries is hostile
      // or broken either way; abort rather than install a bundle with holes.
      LOG_ERR("PCAT", "unsafe bundle entry rejected: %s", item.files[i].c_str());
      return HttpDownloader::FILE_ERROR;
    }
    // Create any intermediate folders for nested files ("assets/icon.bin").
    const size_t slash = dest.find_last_of('/');
    if (slash != std::string::npos) {
      const std::string parent = dest.substr(0, slash);
      if (!Storage.exists(parent.c_str())) Storage.mkdir(parent.c_str());
    }
    const auto result = downloadFile(base + rel, dest + ".new");
    if (result != HttpDownloader::OK) return result;
    staged.push_back(dest);
  }
  // ponytail: a failed swap mid-loop leaves a mixed old/new bundle; a
  // directory-level swap would close that, at the cost of copying the tree.
  for (size_t i = 0; i < staged.size(); i++) {
    if (!Storage.replaceFile((staged[i] + ".new").c_str(), staged[i].c_str())) {
      LOG_ERR("PCAT", "bundle swap failed: %s", staged[i].c_str());
      for (size_t j = i; j < staged.size(); j++) Storage.remove((staged[j] + ".new").c_str());
      complete = true;  // already-swapped files stay; the rollback must not touch them
      return HttpDownloader::FILE_ERROR;
    }
  }
  complete = true;
  emitBookDownloaded(manifestPath, staged.empty() ? "" : staged.front(), item.title);
  // Bundles are how plugins install (plugin-store); pick up any new event
  // subscriptions without a restart. Cheap: a few small manifest reads.
  pluginevents::refreshSubscriptions();
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError PluginCatalogActivity::downloadBook(const Item& item) {
  // Resolve the file URL: either the template itself, or one API hop away.
  std::string fileUrl;
  if (!manifest.dlUrlPath.empty()) {
    String response;
    int status = apiRequest(substitutedRequest(manifest.downloadReq, &item), response);
    // An expired password-grant token: mint a fresh one and retry once.
    if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) {
      status = apiRequest(substitutedRequest(manifest.downloadReq, &item), response);
    }
    if (status < 200 || status >= 300) {
      return HttpDownloader::HTTP_ERROR;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
      return HttpDownloader::HTTP_ERROR;
    }
    fileUrl = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.dlUrlPath));
  } else {
    fileUrl = substituted(manifest.downloadReq.url, &item, pluginhttp::TemplateContext::Url);
  }
  if (fileUrl.empty()) {
    return HttpDownloader::FILE_ERROR;
  }

  const std::string folderDir = downloadDir();
  const char* folder = folderDir.c_str();
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("PCAT", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // Sanitize after substitution so the byte limit applies to the complete
  // filename and does not remove an extension already present in {title}.
  const std::string filename = StringUtils::sanitizeFilename(substituted(manifest.filenameTpl, &item), 120);
  if (filename.empty() || filename.find("..") != std::string::npos || filename.find('/') != std::string::npos) {
    LOG_ERR("PCAT", "unsafe filename rejected: %s", filename.c_str());
    return HttpDownloader::FILE_ERROR;
  }
  std::string dest;
  dest.reserve((haveFolder ? folderDir.size() : 0) + 1 + filename.size());
  if (haveFolder) dest += folder;
  dest += '/';
  dest += filename;
  if (!protectedpaths::isPluginPath(dest)) {
    LOG_ERR("PCAT", "unsafe download dest rejected: %s", dest.c_str());
    return HttpDownloader::FILE_ERROR;
  }

  // url_path already authenticated the JSON hop; the resolved file URL must not
  // inherit those headers (S3 pre-signed GETs reject a second Authorization).
  const auto fetchFile = [&] {
    const std::vector<HttpDownloader::Header> fileHeaders =
        manifest.dlUrlPath.empty() ? substitutedHeaders(manifest.downloadReq.headers, &item)
                                   : std::vector<HttpDownloader::Header>{};
    return downloadFile(fileUrl, dest, substituted(manifest.dlUser, &item), substituted(manifest.dlPass, &item),
                        fileHeaders);
  };
  session.reset();  // free browse TLS before the large file GET
  auto result = fetchFile();
  // The direct GET carries the catalog's token templates (never a pre-signed
  // url_path target): an expired password-grant token gets one mint-and-retry.
  if (result == HttpDownloader::UNAUTHORIZED && manifest.dlUrlPath.empty() && manifest.hasPasswordGrant() &&
      refreshCredentialToken()) {
    result = fetchFile();
  }
  if (result != HttpDownloader::OK) return result;
  invalidateBookCache(dest);

  // Optional per-book sidecar (e.g. a service book id keyed by the file's
  // path hash) so a later sync stage can associate the file with the service.
  if (!manifest.sidecarPath.empty() && !manifest.sidecarBody.empty()) {
    const std::string md5 = md5Hex(dest);
    std::string path = substituted(manifest.sidecarPath, &item, pluginhttp::TemplateContext::Path, &md5, &dest);
    // {dest} is the sanitized on-SD path of the downloaded file, so a sidecar
    // can sit next to it ("{dest}.meta.json" - the book metadata convention);
    // {title} alone cannot express that, since the filename is sanitized.
    // Sidecar paths legitimately contain '/', but the substituted fields must
    // not climb out of the tree or land on a credential store.
    if (!protectedpaths::isPluginPath(path)) {
      LOG_ERR("PCAT", "unsafe sidecar path rejected: %s", path.c_str());
    } else {
      std::string body = substituted(manifest.sidecarBody, &item, pluginhttp::TemplateContext::Json, &md5, &dest);
      if (!pluginhttp::TemplateJsonValidator(body).validate() || body.size() > 4096 ||
          !protectedpaths::isPluginPath(path + ".tmp") || !protectedpaths::isPluginPath(path + ".bak") ||
          !pluginhttp::writeStaged(path + ".tmp", body.data(), body.size()) ||
          !Storage.replaceFile((path + ".tmp").c_str(), path.c_str()))
        LOG_ERR("PCAT", "Sidecar write failed: %s", path.c_str());
    }
  }

  emitBookDownloaded(manifestPath, dest, item.title);
  return HttpDownloader::OK;
}

// Sign-in and completion states precede the shared catalog input handling.
bool PluginCatalogActivity::handleCustomInput() {
  if (state == State::DOWNLOADING && transferTask && transferDone.load()) {
    const auto result = transferResult;
    stopTransfer();
    if (!parseBrowseResponse()) return true;
    session = makeUniqueNoThrow<freeink::SecureHttpClient>();
    finishDownload(result);
    return true;
  }
  if (state == State::AUTH) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::NO_TOKEN;
      requestUpdate();
      return true;
    }
    if (static_cast<long>(millis() - authNextPollMs) >= 0) pollAuth();
    return true;
  }

  if (state == State::NO_TOKEN) {
    // Back first: a header back tap is also a screen tap, which means sign in here.
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitCatalog();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (!wifiConnected())
        launchWifiSelection();
      else
        retryBrowse();
    }
    return true;
  }

  if (state == State::DONE) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tx, ty)) {
      // The just-finished download may have installed/updated a plugin; refresh
      // the install badges so the row no longer reads "Update".
      computeInstallStatus();
      releaseRows();
      state = State::BROWSING;
      requestUpdate();
    }
    return true;
  }

  return CatalogActivity::handleCustomInput();
}

void PluginCatalogActivity::retryBrowse() {
  if (state == State::NO_TOKEN && manifest.hasDeviceCode()) {
    beginAuth();
  } else if (wantsListPicker()) {
    startBrowse();
  } else {
    beginLoading();
    fetchPage(page);
  }
}

// Back on the picker leaves the activity; the base routes it here via
// handleButtons. The catalog states route their Back through exitCatalog()
// instead, landing back on the picker.
void PluginCatalogActivity::onBackButton() {
  if (state == State::ERROR || state == State::CHECK_WIFI || state == State::LOADING) {
    exitCatalog();
    return;
  }
  if (state == State::PLUGIN_PICKER) {
    if (rootMode) {
      onGoHome();  // home launch replaced the home screen (root)
    } else {
      finish();  // Settings launch pushed us
    }
    return;
  }
  if (searchActive) {
    // Leave the results and return to the pre-search view (picker or page 1).
    searchActive = false;
    searchQuery.clear();
    startBrowse();
    return;
  }
  if (manifest.isXmlList() && !browseHistory.empty()) {
    browseCurrentUrl = browseHistory.back();
    browseHistory.pop_back();
    beginLoading();
    fetchPage(page);
  } else if (state == State::BROWSING && currentList >= 0) {
    // Browsing a picked list: Back returns to the list picker, not out.
    currentList = -1;
    startBrowse();
  } else {
    exitCatalog();
  }
}

void PluginCatalogActivity::activateIndex(const int index) {
  if (state == State::PLUGIN_PICKER) {
    if (index < 0 || index >= rowCount()) return;
    if (showOpds && index == 0) {
      app.clearTapFlash();            // the row leaves this screen
      activityManager.goToBrowser();  // replaces this screen with the OPDS browser
      return;
    }
    const PluginRef& plugin = installedPlugins[index - (showOpds ? 1 : 0)];
    auto info = makeUniqueNoThrow<PluginInfoActivity>(renderer, mappedInput, plugin);
    if (!info) {
      LOG_ERR("PCAT", "OOM: plugin info");
      return;
    }
    app.clearTapFlash();
    pickerReturnRow = index;
    startActivityForResult(std::move(info), [this](const ActivityResult& result) {
      if (result.isCancelled || !std::holds_alternative<FilePathResult>(result.data)) {
        enterPluginPicker();
        return;
      }
      manifestPath = std::get<FilePathResult>(result.data).path;
      for (const auto& p : installedPlugins)
        if (p.manifestPath == manifestPath) catalogTitle = p.title;
      enterCatalog();
    });
    return;
  }
  if (state == State::LIST_PICKER) {
    if (index < 0 || index >= static_cast<int>(manifest.browseLists.size())) return;
    app.clearTapFlash();
    currentList = index;
    startBrowse();
    return;
  }
  // The pager rows bracket the items: "Previous page" ahead of them past
  // page 1, "Next page" after them while more pages exist.
  const int itemIndex = index - (prevRowVisible() ? 1 : 0);
  if (itemIndex == -1 || (nextRowVisible() && itemIndex == static_cast<int>(items.size()))) {
    app.clearTapFlash();
    beginLoading();
    fetchPage(itemIndex == -1 ? page - 1 : page + 1);
    return;
  }
  app.clearTapFlash();  // the row leaves this screen (folder, download view)
  activateItem(itemIndex);
}

void PluginCatalogActivity::activateItem(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const Item& item = items[itemIndex];
  if (manifest.isXmlList() && item.isDir) {
    if (browseHistory.size() >= 32) {
      fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
      return;
    }
    browseHistory.reserve(32);
    browseHistory.push_back(browseCurrentUrl);
    browseCurrentUrl = item.url;
    beginLoading();
    fetchPage(page);
    return;
  }
  downloadItem(itemIndex);
}

void PluginCatalogActivity::drawFooter() {
  // The QR is a raw-renderer overlay (FreeInkUI has no QR component); the base
  // render() calls drawFooter() after the app has painted.
  if (state == State::AUTH && authQrRect.width > 0) {
    QrUtils::drawQrCode(renderer, Rect{authQrRect.x, authQrRect.y, authQrRect.width, authQrRect.height}, authVerifyUrl);
  }
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER:
    case State::PLUGIN_PICKER: {
      const int count = rowCount();
      const int prevOff = prevRowVisible() ? 1 : 0;
      const int itemSel = nav.selected - prevOff;
      const char* confirmLabel;
      if (state != State::BROWSING) {
        confirmLabel = count > 0 ? tr(STR_OPEN) : "";
      } else {
        // Folders open; items and the pager rows both fetch from the server.
        const bool onDir =
            manifest.isXmlList() && itemSel >= 0 && itemSel < static_cast<int>(items.size()) && items[itemSel].isDir;
        confirmLabel = count == 0 ? "" : (onDir ? tr(STR_OPEN) : tr(STR_FETCH));
      }
      // On the top row of a searchable catalog the previous-nav slot becomes
      // Search (front Left), mirroring the OPDS browser's side-button search.
      const bool searchable = state == State::BROWSING && manifest.hasSearch() && nav.selected == 0;
      const char* up = searchable ? tr(STR_SEARCH) : (count > 1 ? tr(STR_DIR_UP) : "");
      const char* down = count > 1 ? tr(STR_DIR_DOWN) : "";
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
      break;
    }
    case State::ERROR:
    case State::NO_TOKEN: {
      const bool canSignIn = state == State::NO_TOKEN && manifest.hasDeviceCode();
      labels = mappedInput.mapLabels(tr(STR_BACK), canSignIn ? tr(STR_PLUGIN_SIGN_IN) : tr(STR_RETRY), "", "");
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    default:  // CHECK_WIFI / LOADING / AUTH / DONE (and child-activity handoffs)
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PluginCatalogActivity::buildScreen(UiScreen& screen) {
  const bool listState = state == State::BROWSING || state == State::LIST_PICKER;
  const std::string title = listState ? browsingHeaderLabel() : catalogTitle;
  screenHeader(screen, title.c_str());
  if (buildStatusScreen(screen)) return;

  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER:
    case State::PLUGIN_PICKER:
      buildBrowsingScreen(screen);
      return;
    case State::AUTH:
      buildAuthScreen(screen);
      return;
    case State::DONE:
      catalogCenteredBlock(screen, {{tr(STR_DOWNLOAD_COMPLETE), true}, {statusMessage.c_str()}});
      return;
    case State::NO_TOKEN:
      catalogCenteredBlock(screen,
                           {{manifest.hasDeviceCode() ? tr(STR_PLUGIN_SIGN_IN_HINT) : tr(STR_PLUGIN_NOT_SIGNED_IN)}});
      return;
    default:
      return;
  }
}

// Device-code sign-in: verification URL (text + QR) and the user code. The QR
// bitmap itself is painted by drawFooter() into the rect measured here.
void PluginCatalogActivity::buildAuthScreen(UiScreen& screen) {
  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  fui::TextStyle code = screen.theme().titleText;
  code.align = fui::TextAlign::Center;
  code.bold = true;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = screen.theme().spaceMd;

  screen.target().text(screen.takeTop(lh, gap), authVerifyUrl.c_str(), centered);
  screen.target().text(screen.takeTop(screen.target().lineHeight(code.font), gap), authUserCode.c_str(), code);

  constexpr int16_t qrSize = 180;
  const fui::Rect band = screen.takeTop(qrSize, gap);
  authQrRect = fui::Rect{static_cast<int16_t>(band.x + (band.width - qrSize) / 2), band.y, qrSize, qrSize};

  screen.target().text(screen.takeTop(lh), tr(STR_PLUGIN_AUTH_WAITING), centered);
}

std::string PluginCatalogActivity::browsingHeaderLabel() const {
  std::string label = catalogTitle;
  if (searchActive) {
    label = std::string(tr(STR_SEARCH)) + ": " + searchQuery;
  } else if (state == State::BROWSING && currentList >= 0 &&
             currentList < static_cast<int>(manifest.browseLists.size())) {
    label = manifest.browseLists[currentList].title;
  }
  if (state == State::BROWSING && page > 1) {
    char suffix[16];
    snprintf(suffix, sizeof(suffix), " %d", page);
    label += suffix;
  }
  return label;
}

void PluginCatalogActivity::buildBrowsingScreen(UiScreen& screen) {
  if (rowsDirty) {
    rebuildRowItems();
    rowsDirty = false;
  }

  if (rowItems.empty()) {
    screen.centeredText(state == State::PLUGIN_PICKER ? tr(STR_NO_PLUGINS_INSTALLED) : tr(STR_NO_ENTRIES),
                        screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  if (state == State::PLUGIN_PICKER) {
    // Let a long plugin description wrap onto a second line under the title;
    // the row grows to fit it. maxLines=2 also marks the style caller-owned
    // (an all-default smallText fails textStyleUnset and the list would
    // resubstitute).
    props.subtitleText = screen.theme().smallText;
    props.subtitleText.maxLines = 2;
  }
  syncListViewport(screen, props);
  screen.list(props);
}

// Derives rowItems from the current state's row source: the browse lists
// (LIST_PICKER) or the pager rows bracketing the items (BROWSING). Labels
// point into `manifest`/`items` strings, which outlive the buffer.
void PluginCatalogActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(rowCount());
  const auto addRow = [&](const char* label, const char* subtitle = nullptr, const char* value = nullptr) {
    fui::ListItem row;
    row.label = label;
    row.subtitle = subtitle && subtitle[0] ? subtitle : nullptr;
    row.value = value && value[0] ? value : nullptr;
    row.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(row);
  };
  if (state == State::PLUGIN_PICKER) {
    if (showOpds) addRow(tr(STR_OPDS_BROWSER), tr(STR_OPDS_SERVERS));
    for (const auto& plugin : installedPlugins) {
      //   None       -> web-only hint; chevron only with a readme
      //   Catalog    -> browsable, own description, chevron
      //   Background -> events-only, own description, chevron only with a readme
      const char* subtitle = plugin.description.empty() ? nullptr : plugin.description.c_str();
      switch (plugin.deviceKind) {
        case PluginLocations::DeviceKind::None:
          addRow(plugin.title.c_str(), tr(STR_PLUGIN_WEB_ONLY), plugin.readmePath.empty() ? nullptr : ">");
          break;
        case PluginLocations::DeviceKind::Catalog:
          addRow(plugin.title.c_str(), subtitle, ">");
          break;
        case PluginLocations::DeviceKind::Background:
          addRow(plugin.title.c_str(), subtitle, plugin.readmePath.empty() ? nullptr : ">");
          break;
      }
    }
  } else if (state == State::LIST_PICKER) {
    for (const auto& list : manifest.browseLists) addRow(list.title.c_str());
  } else {
    if (prevRowVisible()) addRow(tr(STR_PREV_PAGE), nullptr, ">");
    for (const auto& entry : items) {
      addRow(entry.title.c_str(), entry.author.c_str(), entry.isDir ? ">" : entry.status.c_str());
    }
    if (nextRowVisible()) addRow(tr(STR_NEXT_PAGE), nullptr, ">");
  }
}

void PluginCatalogActivity::releaseRows() {
  // The app's interaction table holds row indices (and hit rects) for the old
  // rows; stop routing touches against it until the next render.
  closeRouting();
  rowsDirty = true;
}
