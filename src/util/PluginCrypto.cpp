#include "PluginCrypto.h"

#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>
#include <mbedtls/esp_mbedtls_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/rsa.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/des3.h>
#include <wolfssl/wolfcrypt/rc2.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>

#include "PluginCryptoPolicy.h"
#include "TaskWatchdog.h"

namespace plugincrypto {
namespace {
using namespace policy;

void cooperate() {
  resetTaskWatchdogIfSubscribed();
  vTaskDelay(1);
}
int randomCallback(void*, unsigned char* output, size_t length) {
  // Also gives prime generation and RSA blinding a scheduling/watchdog point.
  cooperate();
  return mbedtls_esp_random(nullptr, output, length);
}
void wipe(void* p, size_t n) {
  if (p && n) mbedtls_platform_zeroize(p, n);
}

// Large buffers/contexts are allocated once per operation, not on the web task
// stack. Unlike the SDK vector helper these allocations are all nothrow/checked.
struct Bytes {
  std::unique_ptr<uint8_t[]> data;
  size_t size = 0;
  size_t capacity = 0;
  ~Bytes() { wipe(data.get(), capacity); }
  bool allocate(size_t n) {
    wipe(data.get(), capacity);
    data.reset();
    capacity = size = 0;
    if (n) {
      data = makeUniqueNoThrow<uint8_t[]>(n);
      if (!data) return false;
    }
    capacity = size = n;
    return true;
  }
  const uint8_t* ptr() const {
    static constexpr uint8_t EMPTY = 0;
    return data ? data.get() : &EMPTY;
  }
  View view() const { return {ptr(), size}; }
};
struct Pk {
  mbedtls_pk_context value;
  Pk() { mbedtls_pk_init(&value); }
  ~Pk() { mbedtls_pk_free(&value); }
};
struct Cert {
  mbedtls_x509_crt value;
  Cert() { mbedtls_x509_crt_init(&value); }
  ~Cert() { mbedtls_x509_crt_free(&value); }
};
struct AesContext {
  mbedtls_aes_context value;
  AesContext() { mbedtls_aes_init(&value); }
  ~AesContext() { mbedtls_aes_free(&value); }
};
mbedtls_md_type_t mdType(Hash h) {
  return h == Hash::Sha1     ? MBEDTLS_MD_SHA1
         : h == Hash::Sha256 ? MBEDTLS_MD_SHA256
         : h == Hash::Sha384 ? MBEDTLS_MD_SHA384
                             : MBEDTLS_MD_SHA512;
}
struct Digest {
  mbedtls_md_context_t value;
  Digest() { mbedtls_md_init(&value); }
  ~Digest() { mbedtls_md_free(&value); }
  bool setup(Hash hash, bool hmac = false) {
    const auto* info = mbedtls_md_info_from_type(mdType(hash));
    return info && mbedtls_md_setup(&value, info, hmac) == 0;
  }
  static bool hash(void* context, const uint8_t* a, size_t an, const uint8_t* b, size_t bn, uint8_t* out) {
    auto* self = static_cast<Digest*>(context);
    return mbedtls_md_starts(&self->value) == 0 && (!an || mbedtls_md_update(&self->value, a, an) == 0) &&
           (!bn || mbedtls_md_update(&self->value, b, bn) == 0) && mbedtls_md_finish(&self->value, out) == 0;
  }
  static bool hmac(void* context, const uint8_t* a, size_t an, const uint8_t* b, size_t bn, uint8_t* out) {
    auto* self = static_cast<Digest*>(context);
    return mbedtls_md_hmac_reset(&self->value) == 0 && (!an || mbedtls_md_hmac_update(&self->value, a, an) == 0) &&
           (!bn || mbedtls_md_hmac_update(&self->value, b, bn) == 0) && mbedtls_md_hmac_finish(&self->value, out) == 0;
  }
  static void yield(void*) { cooperate(); }
  KdfCallbacks callbacks() { return {this, hash, hmac, yield}; }
};
struct Work {
  KdfWork kdf;
  uint8_t key[64], iv[16], mac[64];
  ~Work() { wipe(this, sizeof(*this)); }
};
struct Operation {
  const JsonDocument& req;
  JsonDocument& response;
  int& status;
  size_t decoded = 0;
  uint32_t kdfUsed = 0;
  std::unique_ptr<DerWork> derWork;
  bool fail(int code, const char* error, int backend = 0) {
    status = code;
    response.clear();
    response["error"] = error;
    if (backend) response["code"] = backend;
    LOG_ERR("PLUGCRYPTO", "%s (%d)", error, backend);
    return false;
  }
  bool oom() { return fail(503, "out of memory"); }
  bool parserScratch() {
    if (!derWork) derWork = makeUniqueNoThrow<DerWork>();
    return derWork != nullptr || oom();
  }
  bool decodeField(const char* field, Bytes& bytes, bool optional = false) {
    const auto v = req[field];
    if (v.isNull() && optional) return true;
    if (!v.is<const char*>()) return fail(400, "base64 string field required");
    const JsonString text = v.as<JsonString>();
    const std::string_view input(text.c_str(), text.size());
    size_t n = 0;
    if (input.size() > encodedSize(DECODE_CAP)) return fail(413, "decoded input exceeds 8 KiB");
    if (!decodedSize(input, n)) return fail(400, "invalid canonical base64");
    if (n > DECODE_CAP - decoded) return fail(413, "decoded inputs exceed 8 KiB total");
    if (!bytes.allocate(n)) return oom();
    if (!decode(input, bytes.data.get(), n, bytes.size)) return fail(400, "invalid base64");
    decoded += n;
    cooperate();
    return true;
  }
  bool encoded(const char* field, const uint8_t* data, size_t size) {
    const size_t n = encodedSize(size);
    auto out = makeUniqueNoThrow<char[]>(n + 1);
    if (!out) return oom();
    if (!encode(data, size, out.get(), n + 1)) return fail(422, "crypto output exceeds cap");
    // ArduinoJson copies this non-const, temporary buffer and reports OOM.
    response[field] = out.get();
    wipe(out.get(), n + 1);
    return response.overflowed() ? oom() : true;
  }
  bool copy(Bytes& target, View value) {
    if (value.size > DECODE_CAP) return fail(413, "DER output exceeds cap");
    if (!target.allocate(value.size)) return oom();
    if (value.size) memcpy(target.data.get(), value.data, value.size);
    return true;
  }
  bool privateKey(View der, Pk& key) {
    if (!rsaPrivateDer(der)) return fail(400, "private key must be bounded unencrypted RSA DER");
#if MBEDTLS_VERSION_MAJOR >= 3
    const int rc = mbedtls_pk_parse_key(&key.value, der.data, der.size, nullptr, 0, randomCallback, nullptr);
#else
    const int rc = mbedtls_pk_parse_key(&key.value, der.data, der.size, nullptr, 0);
#endif
    if (rc) return fail(422, "private key parse failed", rc);
    const size_t bits = mbedtls_pk_get_bitlen(&key.value);
    if (mbedtls_pk_get_type(&key.value) != MBEDTLS_PK_RSA || bits < 512 || bits > 4096)
      return fail(400, "RSA key must be 512..4096 bits");
    return true;
  }
  bool certificate(View der, Cert& cert) {
    if (!parserScratch()) return false;
    if (!certificateRsaCap(der, true, *derWork)) return fail(400, "certificate must contain bounded RSA DER");
    const int rc = mbedtls_x509_crt_parse_der(&cert.value, der.data, der.size);
    if (rc) return fail(422, "certificate parse failed", rc);
    const size_t bits = mbedtls_pk_get_bitlen(&cert.value.pk);
    if (mbedtls_pk_get_type(&cert.value.pk) != MBEDTLS_PK_RSA || bits < 512 || bits > 4096)
      return fail(400, "RSA certificate must be 512..4096 bits");
    // Deliberately do not validate trust/expiry: caller supplies a generic key,
    // this is not a TLS certificate validator or a DRM certificate endpoint.
    return true;
  }
};

bool heapAdmission(Operation& op, bool heavy, bool bundle) {
  // Conservative admission estimates, NOT a hardware-proven peak guarantee.
  // The already-parsed request is included in current free heap; leave a 50 KiB
  // reserve before backend MPI/X509 allocations. All later allocations checked.
  const size_t need = (bundle ? 112u : heavy ? 96u : 64u) * 1024;
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < need ||
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < (heavy ? 16384u : 12288u))
    return op.fail(503, "insufficient crypto heap reserve");
  return true;
}
[[gnu::noinline]] bool hashOperation(Operation& op, Hash hash) {
  Bytes input;
  if (!op.decodeField("data", input, true)) return false;
  auto ctx = makeUniqueNoThrow<Digest>();
  if (!ctx) return op.oom();
  if (!ctx->setup(hash) || mbedtls_md_starts(&ctx->value)) return op.fail(422, "hash initialization failed");
  for (size_t offset = 0; offset < input.size; offset += 1024) {
    const int rc = mbedtls_md_update(&ctx->value, input.ptr() + offset, std::min(size_t{1024}, input.size - offset));
    if (rc) return op.fail(422, "hash update failed", rc);
    cooperate();
  }
  uint8_t digest[32];
  const int rc = mbedtls_md_finish(&ctx->value, digest);
  if (rc) return op.fail(422, "hash failed", rc);
  if (!op.encoded("data", digest, hashSize(hash))) return false;
  if (hash == Hash::Sha256) {
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    char hex[65];
    for (size_t i = 0; i < 32; ++i) {
      hex[2 * i] = HEX_DIGITS[digest[i] >> 4];
      hex[2 * i + 1] = HEX_DIGITS[digest[i] & 15];
    }
    hex[64] = 0;
    op.response["hex"] = hex;  // preserve existing local convenience field
  }
  wipe(digest, sizeof(digest));
  return !op.response.overflowed() || op.oom();
}
[[gnu::noinline]] bool randomOperation(Operation& op) {
  // Upstream len defaults to 16; preserve the existing local bytes field alias.
  const bool hasLen = !op.req["len"].isNull();
  const bool hasBytes = !op.req["bytes"].isNull();
  const auto field = hasLen ? op.req["len"] : op.req["bytes"];
  if ((hasLen || hasBytes) && !field.is<int>()) return op.fail(400, "random length must be an integer");
  const int n = hasLen || hasBytes ? field.as<int>() : 16;
  if (n < 0 || n > static_cast<int>(RANDOM_CAP)) return op.fail(400, "random length must be 0..4096");
  if (hasLen && hasBytes && (!op.req["bytes"].is<int>() || op.req["bytes"].as<int>() != n))
    return op.fail(400, "conflicting random len/bytes");
  Bytes output;
  if (!output.allocate(n)) return op.oom();
  for (size_t offset = 0; offset < output.size; offset += 256) {
    const int rc = randomCallback(nullptr, output.data.get() + offset, std::min(size_t{256}, output.size - offset));
    if (rc) return op.fail(422, "random generation failed", rc);
  }
  return op.encoded("data", output.ptr(), output.size);
}
[[gnu::noinline]] bool aesOperation(Operation& op, bool encrypt) {
  Bytes key, iv, data;
  if (!op.decodeField("key", key) || !op.decodeField("iv", iv) || !op.decodeField("data", data, true)) return false;
  if (key.size != 16 || iv.size != 16) return op.fail(400, "key/iv must be 16 bytes");
  size_t size = data.size;
  if (encrypt) {
    if (!paddedSize(data.size, size)) return op.fail(413, "AES input exceeds cap");
  } else if (size % 16)
    return op.fail(400, "data not block-aligned");
  Bytes output;
  if (!output.allocate(size)) return op.oom();
  if (data.size) memcpy(output.data.get(), data.ptr(), data.size);
  if (encrypt) memset(output.data.get() + data.size, size - data.size, size - data.size);
  auto aes = makeUniqueNoThrow<AesContext>();
  if (!aes) return op.oom();
  int rc = encrypt ? mbedtls_aes_setkey_enc(&aes->value, key.ptr(), 128)
                   : mbedtls_aes_setkey_dec(&aes->value, key.ptr(), 128);
  if (rc) return op.fail(422, "AES set-key failed", rc);
  for (size_t offset = 0; offset < size; offset += 1024) {
    rc = mbedtls_aes_crypt_cbc(&aes->value, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                               std::min(size_t{1024}, size - offset), iv.data.get(), output.ptr() + offset,
                               output.data.get() + offset);
    if (rc) return op.fail(422, "AES operation failed", rc);
    cooperate();
  }
  // aesdec intentionally returns raw blocks, including any PKCS#7 padding.
  return op.encoded("data", output.ptr(), size);
}
[[gnu::noinline]] bool rsaOperation(Operation& op, bool sign) {
  Bytes key, data;
  if (!op.decodeField("private", key) || !op.decodeField(sign ? "hash" : "data", data)) return false;
  auto pk = makeUniqueNoThrow<Pk>();
  if (!pk) return op.oom();
  if (!op.privateKey(key.view(), *pk)) return false;
  const size_t n = mbedtls_pk_get_len(&pk->value);
  if (n > RSA_CAP || (sign && n != 128)) return op.fail(400, "sign requires RSA-1024; RSA maximum is 4096-bit");
  Bytes input, output;
  if (!input.allocate(n) || !output.allocate(n)) return op.oom();
  if (sign) {
    if (!signBlock(data.ptr(), data.size, input.data.get(), n)) return op.fail(400, "hash must be 20 bytes");
  } else {
    if (!data.size || data.size > n) return op.fail(400, "raw RSA input must fit modulus");
    memset(input.data.get(), 0, n);
    memcpy(input.data.get() + n - data.size, data.ptr(), data.size);
  }
  cooperate();
  const int rc =
      mbedtls_rsa_private(mbedtls_pk_rsa(pk->value), randomCallback, nullptr, input.ptr(), output.data.get());
  cooperate();
  if (rc) return op.fail(422, "raw private RSA operation failed", rc);
  return op.encoded("data", output.ptr(), n);
}
[[gnu::noinline]] bool publicEncrypt(Operation& op) {
  Bytes der, data;
  if (!op.decodeField("cert", der) || !op.decodeField("data", data, true)) return false;
  auto cert = makeUniqueNoThrow<Cert>();
  if (!cert) return op.oom();
  if (!op.certificate(der.view(), *cert)) return false;
  const size_t n = mbedtls_pk_get_len(&cert->value.pk);
  if (n > RSA_CAP || data.size > n - 11) return op.fail(400, "RSA plaintext exceeds modulus minus 11");
  Bytes output;
  if (!output.allocate(n)) return op.oom();
  auto* rsa = mbedtls_pk_rsa(cert->value.pk);
#if MBEDTLS_VERSION_MAJOR >= 3
  int rc = mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
  if (!rc)
    rc = mbedtls_rsa_rsaes_pkcs1_v15_encrypt(rsa, randomCallback, nullptr, data.size, data.ptr(), output.data.get());
#else
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
  const int rc = mbedtls_rsa_rsaes_pkcs1_v15_encrypt(rsa, randomCallback, nullptr, MBEDTLS_RSA_PUBLIC, data.size,
                                                     data.ptr(), output.data.get());
#endif
  if (rc) return op.fail(422, "certificate public encryption failed", rc);
  cooperate();
  return op.encoded("data", output.ptr(), n);
}
size_t derHeader(uint8_t* out, uint8_t tag, size_t n) {
  out[0] = tag;
  if (n < 128) {
    out[1] = n;
    return 2;
  }
  if (n < 256) {
    out[1] = 0x81;
    out[2] = n;
    return 3;
  }
  out[1] = 0x82;
  out[2] = n >> 8;
  out[3] = n;
  return 4;
}
bool wrapPkcs8(Operation& op, View traditional, Bytes& output) {
  static constexpr uint8_t ALG[] = {0x30, 0x0d, 6, 9, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 1, 1, 5, 0};
  Der root;
  if (!one(traditional, root, 0x30) || traditional.size > DECODE_CAP - 32)
    return op.fail(400, "invalid traditional RSA DER");
  // 32-byte allowance covers version, AlgorithmIdentifier and both headers.
  if (!output.allocate(traditional.size + 32)) return op.oom();
  uint8_t h[4];
  const size_t octets = derHeader(h, 4, traditional.size);
  const size_t inner = 3 + sizeof(ALG) + octets + traditional.size;
  size_t pos = derHeader(output.data.get(), 0x30, inner);
  output.data[pos++] = 2;
  output.data[pos++] = 1;
  output.data[pos++] = 0;
  memcpy(output.data.get() + pos, ALG, sizeof(ALG));
  pos += sizeof(ALG);
  memcpy(output.data.get() + pos, h, octets);
  pos += octets;
  memcpy(output.data.get() + pos, traditional.data, traditional.size);
  pos += traditional.size;
  output.size = pos;
  return true;
}
[[gnu::noinline]] bool keygen(Operation& op) {
  // Legacy wire contract: fixed 1024 bits/e=65537, SPKI public, PKCS#8 private.
  // Do not route this through the SDK's unstable ESP fast-math prime generator.
  auto key = makeUniqueNoThrow<Pk>();
  if (!key) return op.oom();
  int rc = mbedtls_pk_setup(&key->value, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (!rc) rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key->value), randomCallback, nullptr, 1024, 65537);
  if (rc) return op.fail(422, "keygen failed", rc);
  Bytes der, wrapped;
  if (!der.allocate(1024)) return op.oom();
  int n = mbedtls_pk_write_pubkey_der(&key->value, der.data.get(), der.size);
  if (n <= 0) return op.fail(422, "SPKI export failed", n);
  if (!op.encoded("public", der.ptr() + der.size - n, n)) return false;
  n = mbedtls_pk_write_key_der(&key->value, der.data.get(), der.size);
  if (n <= 0) return op.fail(422, "private DER export failed", n);
  if (!wrapPkcs8(op, {der.ptr() + der.size - n, static_cast<size_t>(n)}, wrapped)) return false;
  return op.encoded("private", wrapped.ptr(), wrapped.size);
}

// Independent, bounded PKCS#12 adapter. It does not invoke wc_PKCS12_parse:
// that SDK path uses unbounded bag allocations and large MAC stack buffers.
// Definite DER data/encryptedData, key/shrouded-key/X509-cert bags are supported;
// nested/secret/CRL/RC4/signed-data bags fail explicitly rather than ignored.
struct Bundle {
  Operation& op;
  std::string_view password;
  Bytes unicode, key;
  Bytes certs[SAFE_CAP];
  size_t certCount = 0, certBytes = 0, bags = 0;
  Bundle(Operation& operation, std::string_view pw) : op(operation), password(pw) {}
  bool initialize() {
    // Match the SDK's byte-to-BMP conversion exactly (ASCII/byte passwords).
    // No implicit UTF-8 reinterpretation, no hidden allocation in std::string.
    if (!unicode.allocate(2 * password.size() + 2)) return op.oom();
    for (size_t i = 0; i < password.size(); ++i) {
      unicode.data[2 * i] = 0;
      unicode.data[2 * i + 1] = static_cast<uint8_t>(password[i]);
    }
    unicode.data[unicode.size - 2] = unicode.data[unicode.size - 1] = 0;
    return true;
  }
  bool bad() { return op.fail(400, "invalid or unsupported PKCS12 DER/bag/algorithm"); }
  [[gnu::noinline]] bool decrypt(View input, Bytes& output);
  [[gnu::noinline]] bool parseBags(View input);
  [[gnu::noinline]] bool parseSafe(View input);
  [[gnu::noinline]] bool verifyMac(View input, View authenticated);
  [[gnu::noinline]] bool finish();
};
bool derive(Operation& op, Work& work, Digest& digest, const Pbe& pbe, View password, View unicode) {
  if (!digest.setup(pbe.hash, !pbe.pkcs12)) return op.fail(422, "PBE digest initialization failed");
  const size_t blocks = (pbe.keySize + hashSize(pbe.hash) - 1) / hashSize(pbe.hash) + (pbe.pkcs12 ? 1 : 0);
  if (!charge(op.kdfUsed, pbe.iterations, blocks)) return op.fail(413, "PKCS12 KDF work limit exceeded");
  const auto cb = digest.callbacks();
  if (pbe.pkcs12) {
    if (!pkcs12Kdf(cb, work.kdf, pbe.hash, pbe.salt, unicode, pbe.iterations, 1, work.key, pbe.keySize) ||
        !pkcs12Kdf(cb, work.kdf, pbe.hash, pbe.salt, unicode, pbe.iterations, 2, work.iv, pbe.block))
      return op.fail(422, "PKCS12 key derivation failed");
  } else {
    if (mbedtls_md_hmac_starts(&digest.value, password.data, password.size) ||
        !pbkdf2(cb, work.kdf, pbe.hash, pbe.salt, pbe.iterations, work.key, pbe.keySize))
      return op.fail(422, "PBKDF2 failed");
    memcpy(work.iv, pbe.iv.data, pbe.block);
  }
  return true;
}
bool decryptCipher(Operation& op, const Pbe& pbe, Work& work, Bytes& output) {
  if (pbe.cipher == Cipher::Aes) {
    auto ctx = makeUniqueNoThrow<AesContext>();
    if (!ctx) return op.oom();
    int rc = mbedtls_aes_setkey_dec(&ctx->value, work.key, pbe.keySize * 8);
    if (rc) return op.fail(422, "PBE AES set-key failed", rc);
    for (size_t i = 0; i < output.size; i += 1024) {
      rc = mbedtls_aes_crypt_cbc(&ctx->value, MBEDTLS_AES_DECRYPT, std::min(size_t{1024}, output.size - i), work.iv,
                                 output.ptr() + i, output.data.get() + i);
      if (rc) return op.fail(422, "PBE AES failed", rc);
      cooperate();
    }
  } else if (pbe.cipher == Cipher::Des3) {
#ifndef NO_DES3
    auto ctx = makeUniqueNoThrow<Des3>();
    if (!ctx) return op.oom();
    int rc = wc_Des3Init(ctx.get(), nullptr, INVALID_DEVID);
    if (rc) return op.fail(422, "PBE 3DES initialization failed", rc);
    const ScopedCleanup cleanup([&] {
      wc_Des3Free(ctx.get());
      wipe(ctx.get(), sizeof(Des3));
    });
    if (pbe.keySize == 16) memcpy(work.key + 16, work.key, 8);
    rc = wc_Des3_SetKey(ctx.get(), work.key, work.iv, DES_DECRYPTION);
    if (rc) return op.fail(422, "PBE 3DES set-key failed", rc);
    for (size_t i = 0; i < output.size; i += 1024) {
      rc = wc_Des3_CbcDecrypt(ctx.get(), output.data.get() + i, output.ptr() + i,
                              std::min(size_t{1024}, output.size - i));
      if (rc) return op.fail(422, "PBE 3DES failed", rc);
      cooperate();
    }
#else
    return op.fail(422, "PKCS12 3DES disabled in this build");
#endif
  } else {
#ifdef WC_RC2
    auto ctx = makeUniqueNoThrow<Rc2>();
    if (!ctx) return op.oom();
    const ScopedCleanup cleanup([&] { wipe(ctx.get(), sizeof(Rc2)); });
    int rc = wc_Rc2SetKey(ctx.get(), work.key, pbe.keySize, work.iv, pbe.keySize * 8);
    if (rc) return op.fail(422, "PBE RC2 set-key failed", rc);
    for (size_t i = 0; i < output.size; i += 1024) {
      rc =
          wc_Rc2CbcDecrypt(ctx.get(), output.data.get() + i, output.ptr() + i, std::min(size_t{1024}, output.size - i));
      if (rc) return op.fail(422, "PBE RC2 failed", rc);
      cooperate();
    }
#else
    return op.fail(422, "PKCS12 RC2 requires WC_RC2 in all wolfSSL compilation units");
#endif
  }
  if (!unpad(output.data.get(), output.size, pbe.block)) return op.fail(422, "PBE decryption/padding failed");
  return true;
}
bool Bundle::decrypt(View input, Bytes& output) {
  Der algorithm, encrypted;
  if (!take(input, algorithm, 0x30) || !take(input, encrypted) || input.size ||
      (encrypted.tag != 4 && encrypted.tag != 0x80))
    return bad();
  Pbe pbe;
  if (!op.parserScratch()) return false;
  if (!pbeParameters(algorithm.encoded, pbe, *op.derWork)) return bad();
  if (!encrypted.value.size || encrypted.value.size % pbe.block || encrypted.value.size > DECODE_CAP) return bad();
  auto work = makeUniqueNoThrow<Work>();
  auto digest = makeUniqueNoThrow<Digest>();
  if (!work || !digest) return op.oom();
  if (!derive(op, *work, *digest, pbe, {reinterpret_cast<const uint8_t*>(password.data()), password.size()},
              unicode.view()))
    return false;
  if (!op.copy(output, encrypted.value)) return false;
  return decryptCipher(op, pbe, *work, output);
}
bool Bundle::verifyMac(View input, View authenticated) {
  if (!input.size) return true;  // optional MAC permitted by upstream
  Der mac, digestInfo, alg, expected, salt, id;
  if (!one(input, mac, 0x30)) return bad();
  View m = mac.value;
  if (!take(m, digestInfo, 0x30) || !take(m, salt, 4) || !salt.value.size || salt.value.size > 64) return bad();
  uint32_t iterations = 1;
  if (m.size && !integer(m, iterations)) return bad();
  if (m.size) return bad();
  View d = digestInfo.value;
  if (!take(d, alg, 0x30) || !take(d, expected, 4) || d.size) return bad();
  d = alg.value;
  Hash hash;
  if (!take(d, id, 6) || !hashOid(id, hash) || !nullParameters(d) || expected.value.size != hashSize(hash))
    return bad();
  if (!charge(op.kdfUsed, iterations, 1)) return op.fail(413, "PKCS12 MAC work limit exceeded");
  auto work = makeUniqueNoThrow<Work>();
  auto digest = makeUniqueNoThrow<Digest>();
  if (!work || !digest) return op.oom();
  if (!digest->setup(hash) || !pkcs12Kdf(digest->callbacks(), work->kdf, hash, salt.value, unicode.view(), iterations,
                                         3, work->key, hashSize(hash)))
    return op.fail(422, "PKCS12 MAC derivation failed");
  digest.reset();
  digest = makeUniqueNoThrow<Digest>();
  if (!digest) return op.oom();
  if (!digest->setup(hash, true) || mbedtls_md_hmac_starts(&digest->value, work->key, hashSize(hash)))
    return op.fail(422, "PKCS12 MAC initialization failed");
  for (size_t i = 0; i < authenticated.size; i += 1024) {
    if (mbedtls_md_hmac_update(&digest->value, authenticated.data + i, std::min(size_t{1024}, authenticated.size - i)))
      return op.fail(422, "PKCS12 MAC update failed");
    cooperate();
  }
  if (mbedtls_md_hmac_finish(&digest->value, work->mac)) return op.fail(422, "PKCS12 MAC failed");
  unsigned diff = 0;
  for (size_t i = 0; i < hashSize(hash); ++i) diff |= work->mac[i] ^ expected.value.data[i];
  return diff ? op.fail(422, "PKCS12 password/MAC mismatch") : true;
}
bool Bundle::parseBags(View input) {
  Der sequence;
  if (!one(input, sequence, 0x30)) return bad();
  View all = sequence.value;
  while (all.size) {
    if (++bags > BAG_CAP) return op.fail(413, "PKCS12 bag limit exceeded");
    Der bag, id, value;
    if (!take(all, bag, 0x30)) return bad();
    View b = bag.value;
    uint8_t type = 0;
    if (!take(b, id, 6) || !bagOid(id, type) || !take(b, value, 0xa0)) return bad();
    // Optional bag attributes are one opaque bounded SET, not recursively read.
    if (b.size) {
      Der attributes;
      if (!take(b, attributes, 0x31) || b.size) return bad();
    }
    if (type == 1 || type == 2) {
      if (key.size) return op.fail(400, "PKCS12 must contain exactly one key");
      if (type == 1) {
        if (!op.copy(key, value.value)) return false;
      } else {
        Der encrypted;
        if (!one(value.value, encrypted, 0x30)) return bad();
        if (!decrypt(encrypted.value, key)) return false;
      }
      auto parsed = makeUniqueNoThrow<Pk>();
      if (!parsed) return op.oom();
      if (!op.privateKey(key.view(), *parsed)) return false;
    } else if (type == 3) {
      Der certBag, certType, wrapped, cert;
      static constexpr uint8_t X509[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 9, 0x16, 1};
      if (!one(value.value, certBag, 0x30)) return bad();
      View c = certBag.value;
      if (!take(c, certType, 6) || !oid(certType, X509, sizeof(X509)) || !take(c, wrapped, 0xa0) || c.size ||
          !one(wrapped.value, cert, 4))
        return bad();
      if (certCount == SAFE_CAP || cert.value.size > DECODE_CAP - certBytes)
        return op.fail(413, "PKCS12 certificate limit exceeded");
      if (!op.copy(certs[certCount++], cert.value)) return false;
      certBytes += cert.value.size;
    } else
      return bad();
    cooperate();
  }
  return true;
}
bool Bundle::parseSafe(View input) {
  Der sequence;
  if (!one(input, sequence, 0x30)) return bad();
  View all = sequence.value;
  size_t count = 0;
  while (all.size) {
    if (++count > SAFE_CAP) return op.fail(413, "PKCS12 content-info limit exceeded");
    Der info, id, content, value;
    if (!take(all, info, 0x30)) return bad();
    View c = info.value;
    if (!take(c, id, 6) || !take(c, content, 0xa0) || c.size) return bad();
    if (pkcs7Oid(id, 1)) {
      if (!one(content.value, value, 4)) return bad();
      if (!parseBags(value.value)) return false;
    } else if (pkcs7Oid(id, 6)) {
      if (!one(content.value, value, 0x30)) return bad();
      c = value.value;
      uint32_t version = 0;
      Der encryptedInfo, dataId;
      if (!integer(c, version) || version != 0 || !take(c, encryptedInfo, 0x30) || c.size) return bad();
      c = encryptedInfo.value;
      if (!take(c, dataId, 6) || !pkcs7Oid(dataId, 1)) return bad();
      Bytes plain;
      if (!decrypt(c, plain) || !parseBags(plain.view())) return false;
    } else
      return bad();
    cooperate();
  }
  return true;
}
bool Bundle::finish() {
  if (!key.size || !certCount) return op.fail(422, "PKCS12 key/certificate missing");
  auto parsed = makeUniqueNoThrow<Pk>();
  if (!parsed) return op.oom();
  if (!op.privateKey(key.view(), *parsed)) return false;
  size_t match = SAFE_CAP;
  for (size_t i = 0; i < certCount; ++i) {
    auto cert = makeUniqueNoThrow<Cert>();
    if (!cert) return op.oom();
    if (!op.parserScratch()) return false;
    if (!certificateRsaCap(certs[i].view(), false, *op.derWork)) return bad();
    const int rc = mbedtls_x509_crt_parse_der(&cert->value, certs[i].ptr(), certs[i].size);
    if (rc) return op.fail(422, "PKCS12 certificate parse failed", rc);
    if (mbedtls_pk_get_type(&cert->value.pk) == MBEDTLS_PK_RSA) {
#if MBEDTLS_VERSION_MAJOR >= 3
      const int pair = mbedtls_pk_check_pair(&cert->value.pk, &parsed->value, randomCallback, nullptr);
#else
      const int pair = mbedtls_pk_check_pair(&cert->value.pk, &parsed->value);
#endif
      if (!pair) {
        match = i;
        break;
      }
    }
    cooperate();
  }
  if (match == SAFE_CAP) return op.fail(422, "PKCS12 matching certificate missing");
  // Normalize to RSA PKCS#8 regardless of bag's original PKCS#1/PKCS#8 form.
  Bytes traditional, wrapped;
  if (!traditional.allocate(4096)) return op.oom();
  const int n = mbedtls_pk_write_key_der(&parsed->value, traditional.data.get(), traditional.size);
  if (n <= 0) return op.fail(422, "PKCS12 key export failed", n);
  if (!wrapPkcs8(op, {traditional.ptr() + traditional.size - n, static_cast<size_t>(n)}, wrapped)) return false;
  if (wrapped.size + certs[match].size > DECODE_CAP) return op.fail(413, "PKCS12 output pair exceeds 8 KiB");
  return op.encoded("key", wrapped.ptr(), wrapped.size) && op.encoded("cert", certs[match].ptr(), certs[match].size);
}
[[gnu::noinline]] bool pkcs12(Operation& op) {
  Bytes data;
  if (!op.decodeField("data", data)) return false;
  if (!data.size) return op.fail(400, "PKCS12 bundle required");
  const auto passwordField = op.req["password"];
  if (!passwordField.isNull() && !passwordField.is<const char*>()) return op.fail(400, "password must be a string");
  const JsonString pw = passwordField.isNull() ? JsonString("") : passwordField.as<JsonString>();
  if (pw.size() > PASSWORD_CAP || memchr(pw.c_str(), 0, pw.size()))
    return op.fail(400, "password exceeds 128 bytes or contains NUL");
  auto bundle = makeUniqueNoThrow<Bundle>(op, std::string_view(pw.c_str(), pw.size()));
  if (!bundle) return op.oom();
  if (!bundle->initialize()) return false;
  Der root, info, id, wrapped, authenticated;
  if (!one(data.view(), root, 0x30)) return bundle->bad();
  View p = root.value;
  uint32_t version = 0;
  if (!integer(p, version) || version != 3 || !take(p, info, 0x30)) return bundle->bad();
  View i = info.value;
  if (!take(i, id, 6) || !pkcs7Oid(id, 1) || !take(i, wrapped, 0xa0) || i.size || !one(wrapped.value, authenticated, 4))
    return bundle->bad();
  if (!bundle->verifyMac(p, authenticated.value) || !bundle->parseSafe(authenticated.value)) return false;
  return bundle->finish();
}
}  // namespace

bool process(const JsonDocument& req, JsonDocument& response, int& httpStatus) {
  response.clear();
  httpStatus = 200;
  Operation op{req, response, httpStatus};
  if (req.overflowed()) return op.fail(503, "request document out of memory");
  if (!req.is<JsonObjectConst>()) return op.fail(400, "JSON object required");
  if (measureJson(req) > BODY_CAP) return op.fail(413, "crypto JSON exceeds 16 KiB");
  if (!req["op"].is<const char*>()) return op.fail(400, "crypto operation required");
  const JsonString field = req["op"].as<JsonString>();
  const std::string_view name(field.c_str(), field.size());
  const bool bundle = name == "pkcs12";
  const bool heavy = bundle || name == "keygen" || name == "sign" || name == "rsadec" || name == "pubencrypt" ||
                     name == "pubencryptcert";
  if (!heapAdmission(op, heavy, bundle)) return false;
  bool ok = false;
  if (name == "random")
    ok = randomOperation(op);
  else if (name == "sha1" || name == "sha256")
    ok = hashOperation(op, name == "sha1" ? Hash::Sha1 : Hash::Sha256);
  else if (name == "aesenc" || name == "aesdec")
    ok = aesOperation(op, name == "aesenc");
  else if (name == "rsadec" || name == "sign")
    ok = rsaOperation(op, name == "sign");
  else if (name == "keygen")
    ok = keygen(op);
  else if (name == "pubencrypt" || name == "pubencryptcert")
    ok = publicEncrypt(op);
  else if (bundle)
    ok = pkcs12(op);
  else
    return op.fail(400, "unknown crypto operation; no DRM/expiry/book-key API");
  if (ok && (response.overflowed() || measureJson(response) > BODY_CAP))
    return op.fail(503, "crypto response allocation/cap exceeded");
  return ok;
}
}  // namespace plugincrypto
