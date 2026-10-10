#include <KOReaderCredentialStore.h>
#include <KOReaderSyncPayload.h>
#include <SecureHttpClient.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <string>

namespace {
class SyncProfile : public testing::Test {
 protected:
  KOReaderCredentialStore& store = KOREADER_STORE;
  void SetUp() override {
    Storage.reset();
    PersistableStore<KOReaderCredentialStore>::failWrite = false;
    load("{\"serverType\":1}");
    store.resaveRequested = false;
    freeink::SecureHttpClient::nextCode = 200;
    freeink::SecureHttpClient::nextResponse = "{}";
  }
  void load(const char* json) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, json));
    ASSERT_TRUE(store.fromJson(doc.as<JsonVariantConst>()));
  }
  static KOReaderProgress sample() {
    KOReaderProgress progress{};
    progress.document = "document-hash";
    progress.progress = "/body/p[3]/text().5";
    progress.percentage = 0.25f;
    auto& metadata = progress.metadata.emplace();
    metadata.filename = "Book.epub";
    metadata.title = "Book";
    metadata.authors = "Author";
    metadata.isbn = "9781234567890";
    metadata.asin = "B012345678";
    metadata.series = "Series";
    metadata.seriesIndex = 2.5f;
    progress.position.emplace();
    progress.position->xpath = progress.progress;
    progress.position->totalPages = 0;
    return progress;
  }
  static std::string binary(const std::string& url, bool includeUrl = true) {
    std::ostringstream out;
    serialization::writePod(out, uint8_t{1});
    serialization::writeString(out, "legacy-user");
    std::string password = "legacy-pass";
    constexpr uint8_t key[] = {0x4B, 0x4F, 0x52, 0x65, 0x61, 0x64, 0x65, 0x72};
    for (size_t i = 0; i < password.size(); ++i) password[i] ^= key[i % sizeof(key)];
    serialization::writeString(out, password);
    if (includeUrl) {
      serialization::writeString(out, url);
      serialization::writePod(out, uint8_t{1});
    }
    return out.str();
  }
};
}  // namespace

TEST_F(SyncProfile, MissingProfileRetainsDefaultAndEmptyStoredUrl) {
  load("{\"username\":\"user\",\"password\":\"pass\",\"serverUrl\":\"\"}");
  EXPECT_TRUE(store.hasCredentials());
  EXPECT_EQ(store.getBaseUrl(), "https://sync.koreader.rocks:443");
  EXPECT_TRUE(store.getServerUrl().empty());
  EXPECT_EQ(store.getServerType(), KOReaderServerType::KOSYNC);
  EXPECT_EQ(store.getSyncBehavior(), KOReaderSyncBehavior::SMART);
  EXPECT_TRUE(store.resaveRequested);
}

TEST_F(SyncProfile, LegacyDetectionPreservesPortsPathsAndCustomUrls) {
  for (const char* url : {"https://sync.crosspointreader.com", "http://sync.crosspointreader.com:8080/",
                          "sync.crosspointreader.com/path/"}) {
    SCOPED_TRACE(url);
    JsonDocument doc;
    doc["serverUrl"] = url;
    ASSERT_TRUE(store.fromJson(doc.as<JsonVariantConst>()));
    EXPECT_EQ(store.getServerType(), KOReaderServerType::CROSSPOINT);
    EXPECT_EQ(store.getServerUrl(), url);
  }
  for (const char* url : {"https://sync.crosspointreader.com.evil", "https://custom.example/", ""}) {
    SCOPED_TRACE(url);
    JsonDocument doc;
    doc["serverUrl"] = url;
    ASSERT_TRUE(store.fromJson(doc.as<JsonVariantConst>()));
    EXPECT_EQ(store.getServerType(), KOReaderServerType::KOSYNC);
    EXPECT_EQ(store.getServerUrl(), url);
  }
}

TEST_F(SyncProfile, ExplicitProfileIsIndependentOfUrlAndRoundTrips) {
  for (auto type : {KOReaderServerType::CROSSPOINT, KOReaderServerType::KOSYNC, KOReaderServerType::OTHER}) {
    store.setServerType(type);
    store.setServerUrl("https://sync.crosspointreader.com");
    JsonDocument doc;
    store.toJson(doc);
    ASSERT_EQ(doc["serverType"].as<unsigned>(), static_cast<unsigned>(type));
    ASSERT_TRUE(store.fromJson(doc.as<JsonVariantConst>()));
    EXPECT_EQ(store.getServerType(), type);
    EXPECT_EQ(store.supportsRichProgress(), type != KOReaderServerType::KOSYNC);
    EXPECT_EQ(store.supportsExtendedMetadata(), type == KOReaderServerType::OTHER);
  }
  store.setServerType(KOReaderServerType::OTHER);
  store.setServerUrl("");
  EXPECT_EQ(store.getServerType(), KOReaderServerType::OTHER);
  EXPECT_EQ(store.getBaseUrl(), "https://sync.koreader.rocks:443");
}

TEST_F(SyncProfile, InvalidProfileTypesFallBackToStrictKOSync) {
  for (const char* json : {"{\"serverType\":3}", "{\"serverType\":-1}", "{\"serverType\":256}", "{\"serverType\":1.5}",
                           "{\"serverType\":\"OTHER\"}", "{\"serverType\":true}"}) {
    SCOPED_TRACE(json);
    store.setServerType(KOReaderServerType::OTHER);
    store.resaveRequested = false;
    load(json);
    EXPECT_EQ(store.getServerType(), KOReaderServerType::KOSYNC);
    EXPECT_TRUE(store.resaveRequested);
  }
  store.setServerType(static_cast<KOReaderServerType>(255));
  EXPECT_EQ(store.getServerType(), KOReaderServerType::KOSYNC);
}

TEST_F(SyncProfile, LegacyBinaryMigrationKeepsCredentialsUrlAndMatchMethod) {
  for (const char* url : {"", "https://sync.crosspointreader.com:443/", "http://custom.example/"}) {
    SCOPED_TRACE(url);
    Storage.reset();
    Storage.files["/.crosspoint/koreader.bin"] = binary(url);
    ASSERT_TRUE(store.loadFromFile());
    EXPECT_EQ(store.getUsername(), "legacy-user");
    EXPECT_EQ(store.getPassword(), "legacy-pass");
    EXPECT_EQ(store.getServerUrl(), url);
    EXPECT_EQ(store.getMatchMethod(), DocumentMatchMethod::BINARY);
    EXPECT_EQ(store.getServerType(),
              store.usesCrossPointSyncServer() ? KOReaderServerType::CROSSPOINT : KOReaderServerType::KOSYNC);
    EXPECT_TRUE(Storage.exists(KOReaderCredentialStore::getFilePath()));
    EXPECT_TRUE(Storage.exists("/.crosspoint/koreader.bin.bak"));
    EXPECT_FALSE(Storage.exists("/.crosspoint/koreader.bin"));
  }
}

TEST_F(SyncProfile, ShortLegacyBinaryRetainsOldDefaults) {
  store.setServerType(KOReaderServerType::OTHER);
  Storage.files["/.crosspoint/koreader.bin"] = binary("", false);
  ASSERT_TRUE(store.loadFromFile());
  EXPECT_EQ(store.getPassword(), "legacy-pass");
  EXPECT_TRUE(store.getServerUrl().empty());
  EXPECT_EQ(store.getMatchMethod(), DocumentMatchMethod::FILENAME);
  EXPECT_EQ(store.getServerType(), KOReaderServerType::KOSYNC);
}

TEST_F(SyncProfile, FailedMigrationSaveKeepsOriginalBinary) {
  Storage.files["/.crosspoint/koreader.bin"] = binary("");
  PersistableStore<KOReaderCredentialStore>::failWrite = true;
  EXPECT_FALSE(store.loadFromFile());
  EXPECT_TRUE(Storage.exists("/.crosspoint/koreader.bin"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/koreader.bin.bak"));
  EXPECT_EQ(Storage.renames, 0u);
}

TEST_F(SyncProfile, ProfileControlsUploadAndDownloadExtensions) {
  store.setSendMetadata(true);
  auto progress = sample();
  for (auto type : {KOReaderServerType::CROSSPOINT, KOReaderServerType::KOSYNC, KOReaderServerType::OTHER}) {
    store.setServerType(type);
    JsonDocument doc;
    ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
    EXPECT_EQ(!doc["position"].isNull(), type != KOReaderServerType::KOSYNC);
    EXPECT_EQ(!doc["metadata"]["isbn"].isNull(), type == KOReaderServerType::OTHER);
    EXPECT_EQ(!doc["metadata"]["asin"].isNull(), type == KOReaderServerType::OTHER);
    EXPECT_EQ(!doc["metadata"]["series"].isNull(), type == KOReaderServerType::OTHER);
    EXPECT_EQ(!doc["metadata"]["series_index"].isNull(), type == KOReaderServerType::OTHER);
    EXPECT_EQ(doc["metadata"]["title"].as<std::string>(), "Book");
    if (type != KOReaderServerType::KOSYNC) {
      EXPECT_EQ(doc["position"]["pages"].as<unsigned>(), 1u);
    }
    KOReaderProgress decoded{};
    // Read a response with every extension to test inbound enforcement too.
    doc["position"]["pages"] = 10;
    doc["metadata"]["isbn"] = "123";
    koreaderSync::readProgressPayload(doc.as<JsonVariantConst>(), decoded, store);
    EXPECT_EQ(decoded.position.has_value(), type != KOReaderServerType::KOSYNC);
    ASSERT_TRUE(decoded.metadata.has_value());
    EXPECT_EQ(!decoded.metadata->isbn.empty(), type == KOReaderServerType::OTHER);
  }
}

TEST_F(SyncProfile, SendMetadataDisablesBothNativeMetadataAndSidecar) {
  store.setServerType(KOReaderServerType::OTHER);
  auto progress = sample();
  progress.metadata->extraJson = "{\"book_id\":\"service-id\"}";
  JsonDocument doc;
  ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
  EXPECT_TRUE(doc["metadata"].isNull());
  doc["metadata"]["title"] = "remote";
  progress.metadata.emplace();
  koreaderSync::readProgressPayload(doc.as<JsonVariantConst>(), progress, store);
  EXPECT_FALSE(progress.metadata.has_value());
}

TEST_F(SyncProfile, FlatSidecarRetainsTypesAndCannotOverrideAnyNativeKey) {
  store.setSendMetadata(true);
  auto progress = sample();
  progress.metadata->extraJson = R"({"filename":"evil","title":"evil","authors":"evil","isbn":"evil",
    "asin":"evil","series":"evil","series_index":99,"document":"evil","progress":"evil",
    "position":true,"book_id":"00123","count":123,"big":18446744073709551615,"ratio":1.5,
    "enabled":true,"nothing":null,"nested":{"x":1},"array":[1]})";
  for (auto type : {KOReaderServerType::CROSSPOINT, KOReaderServerType::KOSYNC, KOReaderServerType::OTHER}) {
    store.setServerType(type);
    JsonDocument doc;
    ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
    const auto metadata = doc["metadata"];
    EXPECT_EQ(metadata["title"].as<std::string>(), "Book");
    EXPECT_EQ(metadata["filename"].as<std::string>(), "Book.epub");
    EXPECT_EQ(metadata["authors"].as<std::string>(), "Author");
    EXPECT_EQ(metadata["book_id"].as<std::string>(), "00123");
    EXPECT_TRUE(metadata["count"].is<int>());
    EXPECT_EQ(metadata["count"].as<int>(), 123);
    EXPECT_EQ(metadata["big"].as<uint64_t>(), UINT64_MAX);
    EXPECT_DOUBLE_EQ(metadata["ratio"].as<double>(), 1.5);
    EXPECT_TRUE(metadata["enabled"].is<bool>());
    EXPECT_TRUE(metadata["enabled"].as<bool>());
    EXPECT_TRUE(metadata["nested"].isNull());
    EXPECT_TRUE(metadata["array"].isNull());
    EXPECT_TRUE(metadata["nothing"].isNull());
    EXPECT_TRUE(metadata["document"].isNull());
    EXPECT_TRUE(metadata["position"].isNull());
    if (type == KOReaderServerType::OTHER) {
      EXPECT_EQ(metadata["isbn"].as<std::string>(), "9781234567890");
      EXPECT_FLOAT_EQ(metadata["series_index"].as<float>(), 2.5f);
    } else {
      EXPECT_TRUE(metadata["isbn"].isNull());
      EXPECT_TRUE(metadata["series_index"].isNull());
    }
  }
}

TEST_F(SyncProfile, SidecarFillsMissingExtendedFieldsOnlyForOtherProfile) {
  store.setSendMetadata(true);
  store.setServerType(KOReaderServerType::OTHER);
  auto progress = sample();
  progress.metadata->isbn.clear();
  progress.metadata->seriesIndex.reset();
  progress.metadata->extraJson = "{\"isbn\":\"sidecar\",\"series_index\":7}";
  JsonDocument doc;
  ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
  EXPECT_EQ(doc["metadata"]["isbn"].as<std::string>(), "sidecar");
  EXPECT_EQ(doc["metadata"]["series_index"].as<int>(), 7);
  for (auto type : {KOReaderServerType::CROSSPOINT, KOReaderServerType::KOSYNC}) {
    store.setServerType(type);
    ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
    EXPECT_TRUE(doc["metadata"]["isbn"].isNull());
    EXPECT_TRUE(doc["metadata"]["series_index"].isNull());
  }
}

TEST_F(SyncProfile, InvalidOrOversizeSidecarsAreIgnoredAndTwoKiBLimitIsInclusive) {
  store.setSendMetadata(true);
  auto progress = sample();
  for (const char* invalid : {"[1,2]", "null", "1", "{broken", "{\"nested\":{\"deeper\":{\"x\":1}}}"}) {
    progress.metadata->extraJson = invalid;
    JsonDocument doc;
    ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
    EXPECT_EQ(doc["metadata"].as<JsonObject>().size(), 3u);
  }
  progress.metadata->extraJson = "{\"book_id\":\"id\"}";
  progress.metadata->extraJson.resize(koreaderSync::MAX_SIDECAR_BYTES, ' ');
  JsonDocument doc;
  ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
  EXPECT_EQ(doc["metadata"]["book_id"].as<std::string>(), "id");
  progress.metadata->extraJson.push_back(' ');
  ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
  EXPECT_TRUE(doc["metadata"]["book_id"].isNull());
}

TEST_F(SyncProfile, NonfiniteSeriesIndexAndOversizeXPathAreOmitted) {
  store.setServerType(KOReaderServerType::OTHER);
  store.setSendMetadata(true);
  auto progress = sample();
  progress.metadata->seriesIndex = std::numeric_limits<float>::infinity();
  progress.position->xpath.assign(121, 'x');
  JsonDocument doc;
  ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
  EXPECT_TRUE(doc["metadata"]["series_index"].isNull());
  EXPECT_TRUE(doc["position"]["xpath"].isNull());
  progress.position->xpath.resize(120);
  ASSERT_TRUE(koreaderSync::writeProgressPayload(doc, progress, store));
  EXPECT_EQ(doc["position"]["xpath"].as<std::string>().size(), 120u);
}

TEST_F(SyncProfile, HttpClientUsesProfileAndMetadataFlagAtProtocolBoundary) {
  store.setCredentials("user", "pass");
  auto progress = sample();
  progress.metadata->extraJson = "{\"book_id\":\"custom-book\"}";
  EXPECT_EQ(KOReaderSyncClient::updateProgress(progress), KOReaderSyncClient::OK);
  EXPECT_EQ(freeink::SecureHttpClient::lastUrl, "https://sync.koreader.rocks:443/syncs/progress");
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, freeink::SecureHttpClient::lastBody));
  EXPECT_TRUE(doc["metadata"].isNull());
  EXPECT_TRUE(doc["position"].isNull());

  store.setServerType(KOReaderServerType::OTHER);
  store.setSendMetadata(true);
  EXPECT_EQ(KOReaderSyncClient::updateProgress(progress), KOReaderSyncClient::OK);
  ASSERT_FALSE(deserializeJson(doc, freeink::SecureHttpClient::lastBody));
  EXPECT_EQ(doc["metadata"]["book_id"].as<std::string>(), "custom-book");
  EXPECT_EQ(doc["metadata"]["isbn"].as<std::string>(), "9781234567890");
  EXPECT_FALSE(doc["position"].isNull());

  freeink::SecureHttpClient::nextResponse = freeink::SecureHttpClient::lastBody;
  store.setServerType(KOReaderServerType::KOSYNC);
  KOReaderProgress fetched{};
  ASSERT_EQ(KOReaderSyncClient::getProgress("document-hash", fetched), KOReaderSyncClient::OK);
  EXPECT_FALSE(fetched.position.has_value());
  ASSERT_TRUE(fetched.metadata.has_value());
  EXPECT_TRUE(fetched.metadata->isbn.empty());
}
