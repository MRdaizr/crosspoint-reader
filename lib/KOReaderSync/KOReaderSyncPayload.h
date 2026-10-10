#pragma once

#include <ArduinoJson.h>

#include <cstddef>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"

namespace koreaderSync {
constexpr size_t MAX_SIDECAR_BYTES = 2 * 1024;

// Shared with host tests so the protocol boundary is verified without TLS.
bool writeProgressPayload(JsonDocument& doc, const KOReaderProgress& progress, const KOReaderCredentialStore& store);
void readProgressPayload(JsonVariantConst doc, KOReaderProgress& progress, const KOReaderCredentialStore& store);
}  // namespace koreaderSync
