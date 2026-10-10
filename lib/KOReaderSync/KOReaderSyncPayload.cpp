#include "KOReaderSyncPayload.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace {
bool reservedMetadataKey(const char* key) {
  static constexpr const char* KEYS[] = {"filename", "title",     "authors",   "document", "progress", "percentage",
                                         "device",   "device_id", "timestamp", "metadata", "position"};
  for (const char* reserved : KEYS) {
    if (strcmp(key, reserved) == 0) return true;
  }
  return false;
}

bool extendedMetadataKey(const char* key) {
  return strcmp(key, "isbn") == 0 || strcmp(key, "asin") == 0 || strcmp(key, "series") == 0 ||
         strcmp(key, "series_index") == 0;
}

void mergeSidecar(JsonObject metadata, const std::string& json, bool allowExtended) {
  if (json.empty() || json.size() > koreaderSync::MAX_SIDECAR_BYTES) return;
  // This temporary document exists only while building the request, before TLS.
  JsonDocument extra;
  if (deserializeJson(extra, json, DeserializationOption::NestingLimit(2)) || !extra.is<JsonObject>()) return;
  for (JsonPairConst pair : extra.as<JsonObjectConst>()) {
    const char* key = pair.key().c_str();
    if (reservedMetadataKey(key) || (!allowExtended && extendedMetadataKey(key)) || !metadata[key].isNull()) continue;
    const JsonVariantConst value = pair.value();
    if (value.is<const char*>() || value.is<bool>() || value.is<int64_t>() || value.is<uint64_t>() ||
        (value.is<double>() && std::isfinite(value.as<double>()))) {
      metadata[key] = value;
    }
  }
}
}  // namespace

bool koreaderSync::writeProgressPayload(JsonDocument& doc, const KOReaderProgress& progress,
                                        const KOReaderCredentialStore& store) {
  doc.clear();
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = "CrossPoint";
  doc["device_id"] = "crosspoint-reader";

  if (store.getSendMetadata() && progress.metadata.has_value()) {
    const auto& value = *progress.metadata;
    auto metadata = doc["metadata"].to<JsonObject>();
    metadata["filename"] = value.filename;
    metadata["title"] = value.title;
    metadata["authors"] = value.authors;
    if (store.supportsExtendedMetadata()) {
      if (!value.isbn.empty()) metadata["isbn"] = value.isbn;
      if (!value.asin.empty()) metadata["asin"] = value.asin;
      if (!value.series.empty()) metadata["series"] = value.series;
      if (value.seriesIndex.has_value() && std::isfinite(*value.seriesIndex))
        metadata["series_index"] = *value.seriesIndex;
    }
    mergeSidecar(metadata, value.extraJson, store.supportsExtendedMetadata());
  }

  if (store.supportsRichProgress() && progress.position.has_value()) {
    const auto& position = *progress.position;
    auto rich = doc["position"].to<JsonObject>();
    rich["pctQ"] = position.pctQ;
    rich["spine"] = position.spineIndex;
    rich["page"] = position.pageNumber;
    rich["pages"] = position.totalPages > 0 ? position.totalPages : 1;
    if (position.paragraphIndex.has_value()) rich["para"] = *position.paragraphIndex;
    if (!position.xpath.empty() && position.xpath.size() <= 120) rich["xpath"] = position.xpath;
  }
  return !doc.overflowed();
}

void koreaderSync::readProgressPayload(JsonVariantConst doc, KOReaderProgress& progress,
                                       const KOReaderCredentialStore& store) {
  progress.progress = doc["progress"].as<std::string>();
  progress.percentage = doc["percentage"].as<float>();
  progress.device = doc["device"].as<std::string>();
  progress.deviceId = doc["device_id"].as<std::string>();
  progress.timestamp = doc["timestamp"].as<int64_t>();
  progress.metadata.reset();
  if (store.getSendMetadata()) {
    const auto metadata = doc["metadata"].as<JsonObjectConst>();
    if (!metadata.isNull()) {
      auto& value = progress.metadata.emplace();
      value.filename = metadata["filename"] | "";
      value.title = metadata["title"] | "";
      value.authors = metadata["authors"] | "";
      if (store.supportsExtendedMetadata()) {
        value.isbn = metadata["isbn"] | "";
        value.asin = metadata["asin"] | "";
        value.series = metadata["series"] | "";
        if (metadata["series_index"].is<float>() && std::isfinite(metadata["series_index"].as<float>())) {
          value.seriesIndex = metadata["series_index"].as<float>();
        }
      }
    }
  }

  progress.position.reset();
  if (store.supportsRichProgress()) {
    const auto position = doc["position"].as<JsonObjectConst>();
    if (!position.isNull()) {
      KOReaderRichPosition value;
      value.pctQ = position["pctQ"].as<uint32_t>();
      value.spineIndex = position["spine"].as<uint16_t>();
      value.pageNumber = position["page"].as<uint16_t>();
      const uint16_t pages = position["pages"].as<uint16_t>();
      value.totalPages = pages > 0 ? pages : 1;
      const uint16_t paragraph = position["para"].as<uint16_t>();
      if (paragraph > 0) value.paragraphIndex = paragraph;
      const char* xpath = position["xpath"].as<const char*>();
      if (xpath) value.xpath = xpath;
      progress.position = std::move(value);
    }
  }
}
