#pragma once

#include <ArduinoJson.h>

namespace plugincrypto {
// Pure crypto dispatcher, NOT an authorization boundary. The caller must first
// authenticate/approve the plugin and read at most 16 KiB of JSON. No SD, DRM,
// expiry, book-key service, settings or TLS policy is accessed here.
// Returns true for success (HTTP 200); otherwise response contains an explicit
// error and httpStatus is 400/413/422/503. Run serially on the web task, not on a
// render/ISR path. RSA-1024 generation/signing is legacy upstream compatibility.
bool process(const JsonDocument& req, JsonDocument& response, int& httpStatus);
}  // namespace plugincrypto
