#include <wolfssl/wolfcrypt/des3.h>
#include <wolfssl/wolfcrypt/rc2.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "util/PluginCryptoPolicy.h"

using namespace plugincrypto::policy;
static unsigned checks = 0;
#define CHECK(expr)                                           \
  do {                                                        \
    ++checks;                                                 \
    if (!(expr)) {                                            \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); \
      std::exit(1);                                           \
    }                                                         \
  } while (false)
using Vec = std::vector<uint8_t>;
static Vec hex(std::string_view text) {
  Vec out;
  out.reserve(text.size() / 2);
  auto digit = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
  };
  for (size_t i = 0; i < text.size(); i += 2) out.push_back(digit(text[i]) * 16 + digit(text[i + 1]));
  return out;
}
static View view(const Vec& data) { return {data.data(), data.size()}; }
static Vec tlv(uint8_t tag, const Vec& data) {
  Vec out;
  out.reserve(data.size() + 4);
  out.push_back(tag);
  if (data.size() < 128)
    out.push_back(data.size());
  else if (data.size() < 256) {
    out.push_back(0x81);
    out.push_back(data.size());
  } else {
    out.push_back(0x82);
    out.push_back(data.size() >> 8);
    out.push_back(data.size());
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}
static Vec join(std::initializer_list<Vec> values) {
  Vec out;
  size_t n = 0;
  for (const auto& value : values) n += value.size();
  out.reserve(n);
  for (const auto& value : values) out.insert(out.end(), value.begin(), value.end());
  return out;
}
struct Backend {
  Hash type = Hash::Sha1;
  std::string_view password;
  uint32_t yields = 0;
  bool fail = false;
  static bool digest(void* opaque, const uint8_t* a, size_t an, const uint8_t* b, size_t bn, uint8_t* out) {
    auto& ctx = *static_cast<Backend*>(opaque);
    if (ctx.fail) return false;
    if (ctx.type == Hash::Sha1) {
      wc_Sha sha;
      if (wc_InitSha(&sha)) return false;
      const int rc =
          (an ? wc_ShaUpdate(&sha, a, an) : 0) || (bn ? wc_ShaUpdate(&sha, b, bn) : 0) || wc_ShaFinal(&sha, out);
      wc_ShaFree(&sha);
      return rc == 0;
    }
    wc_Sha256 sha;
    if (wc_InitSha256(&sha)) return false;
    const int rc =
        (an ? wc_Sha256Update(&sha, a, an) : 0) || (bn ? wc_Sha256Update(&sha, b, bn) : 0) || wc_Sha256Final(&sha, out);
    wc_Sha256Free(&sha);
    return rc == 0;
  }
  static bool hmac(void* opaque, const uint8_t* a, size_t an, const uint8_t* b, size_t bn, uint8_t* out) {
    auto& ctx = *static_cast<Backend*>(opaque);
    // Independent HMAC adapter for fixtures; policy owns the actual PBKDF2.
    Vec inner(64, 0x36), outer(64, 0x5c);
    inner.reserve(64 + an + bn);
    outer.reserve(64 + hashSize(ctx.type));
    CHECK(ctx.password.size() <= 64);
    for (size_t i = 0; i < ctx.password.size(); ++i) {
      inner[i] ^= ctx.password[i];
      outer[i] ^= ctx.password[i];
    }
    if (an) inner.insert(inner.end(), a, a + an);
    if (bn) inner.insert(inner.end(), b, b + bn);
    uint8_t d[32];
    if (!digest(opaque, inner.data(), inner.size(), nullptr, 0, d)) return false;
    outer.insert(outer.end(), d, d + hashSize(ctx.type));
    return digest(opaque, outer.data(), outer.size(), nullptr, 0, out);
  }
  static void yield(void* opaque) { ++static_cast<Backend*>(opaque)->yields; }
  KdfCallbacks callbacks() { return {this, digest, hmac, yield}; }
};
static void testBase64() {
  size_t n = 999;
  CHECK(decodedSize("", n) && n == 0);
  CHECK(decodedSize("AA==", n) && n == 1);
  CHECK(decodedSize("AQI=", n) && n == 2);
  for (auto invalid : {"A", "AA", "AAA", "A===", "=AAA", "AA=A", "====", "AB==", "AAB=", "AA==\n", "AA-_", "AA A"})
    CHECK(!decodedSize(invalid, n));
  CHECK(!decodedSize(std::string_view("AA\0=", 4), n));
  for (size_t size : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{63}, size_t{4096}, size_t{8191}, DECODE_CAP}) {
    Vec data(size), back(size);
    for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(i * 71);
    std::string encoded(encodedSize(size) + 1, '\0');
    CHECK(encode(data.data(), size, encoded.data(), encoded.size()));
    encoded.resize(encodedSize(size));
    CHECK(decode(encoded, back.data(), back.size(), n) && n == size && data == back);
    if (size) CHECK(!decode(encoded, back.data(), size - 1, n));
  }
  CHECK(!decodedSize(std::string(encodedSize(DECODE_CAP) + 4, 'A'), n));
  uint8_t data[] = {0, 1, 2};
  char out[5];
  CHECK(!encode(data, sizeof(data), out, 4));
}
static void testDerAndPadding() {
  size_t n = 0;
  CHECK(paddedSize(0, n) && n == 16);
  CHECK(paddedSize(16, n) && n == 32);
  CHECK(paddedSize(DECODE_CAP, n) && n == DECODE_CAP + 16);
  CHECK(!paddedSize(DECODE_CAP + 1, n));
  Vec data(16, 16);
  n = 16;
  CHECK(unpad(data.data(), n, 16) && n == 0);
  data.assign(16, 1);
  n = 16;
  CHECK(unpad(data.data(), n, 16) && n == 15);
  data[15] = 0;
  n = 16;
  CHECK(!unpad(data.data(), n, 16));
  data[15] = 17;
  CHECK(!unpad(data.data(), n, 16));
  data[15] = 2;
  data[14] = 1;
  CHECK(!unpad(data.data(), n, 16));
  data.assign(8, 8);
  n = 8;
  CHECK(unpad(data.data(), n, 8) && n == 0);
  std::array<uint8_t, 128> signature;
  std::array<uint8_t, 20> digest{};
  digest.fill(0x55);
  CHECK(signBlock(digest.data(), digest.size(), signature.data(), signature.size()));
  CHECK(signature[0] == 0 && signature[1] == 1 && signature[106] == 255 && signature[107] == 0 &&
        signature[108] == 0x55);
  CHECK(!signBlock(digest.data(), 19, signature.data(), signature.size()));
  CHECK(!signBlock(digest.data(), 20, signature.data(), 127));
  Der root;
  for (const auto& invalid :
       {hex("30800000"), hex("308100"), hex("3082007f"), hex("3083ffffff"), hex("1f00"), hex("3002ff")})
    CHECK(!one(view(invalid), root, 0x30));
  Vec keyBody = hex("020100");
  keyBody.reserve(4096);
  for (unsigned i = 0; i < 8; ++i) {
    const auto value = hex("020101");
    keyBody.insert(keyBody.end(), value.begin(), value.end());
  }
  const auto traditional = tlv(0x30, keyBody);
  CHECK(rsaTraditional(view(traditional)) && rsaPrivateDer(view(traditional)));
  const auto algorithm = hex("300d06092a864886f70d0101010500");
  const auto privateKey = tlv(0x30, join({hex("020100"), algorithm, tlv(4, traditional)}));
  CHECK(rsaPrivateDer(view(privateKey)));
  CHECK(!rsaPrivateDer(view(tlv(0x30, join({algorithm, tlv(4, traditional)})))));  // encrypted-key shape
  Vec huge(514, 0x11);
  huge[0] = 1;
  const auto hugeKey = tlv(0x30, join({hex("020100"), tlv(2, huge), keyBody}));
  CHECK(!rsaPrivateDer(view(hugeKey)));
  Vec negative = privateKey;
  negative.push_back(0);
  CHECK(!rsaPrivateDer(view(negative)));
  // Deterministic malformed input sweep: if a TLV is accepted its views always
  // remain within the original array (checked by ASAN/UBSAN too).
  uint32_t state = 0x493759u;
  for (size_t trial = 0; trial < 4096; ++trial) {
    Vec fuzz(1 + trial % 80);
    for (auto& byte : fuzz) {
      state = state * 1664525u + 1013904223u;
      byte = state >> 24;
    }
    View v = view(fuzz);
    if (take(v, root))
      CHECK(root.encoded.size + v.size == fuzz.size() && root.value.size <= root.encoded.size);
    else
      CHECK(true);
  }
}
static void testPbe() {
  auto scratch = std::unique_ptr<DerWork>(new (std::nothrow) DerWork{});
  CHECK(scratch != nullptr);
  const auto legacy = hex("301c060a2a864886f70d010c0103300e04081234567890abcdef02020800");
  Pbe p;
  CHECK(pbeParameters(view(legacy), p, *scratch));
  CHECK(p.pkcs12 && p.iterations == 2048 && p.keySize == 24 && p.block == 8 && p.cipher == Cipher::Des3);
  Vec rc2 = legacy;
  rc2[13] = 6;
  CHECK(pbeParameters(view(rc2), p, *scratch) && p.cipher == Cipher::Rc2 && p.keySize == 5);
  Vec invalid = legacy;
  invalid[invalid.size() - 2] = 0x7f;
  invalid.back() = 0xff;
  CHECK(!pbeParameters(view(invalid), p, *scratch));
  const auto modern =
      hex("305706092a864886f70d01050d304a302906092a864886f70d01050c301c04081234567890abcdef02020800300c06082a864886f70d"
          "02090500301d060960864801650304012a0410000102030405060708090a0b0c0d0e0f");
  CHECK(pbeParameters(view(modern), p, *scratch));
  CHECK(!p.pkcs12 && p.hash == Hash::Sha256 && p.keySize == 32 && p.iterations == 2048 && p.iv.size == 16);
  uint32_t used = 0;
  CHECK(charge(used, 10000, 3) && used == KDF_WORK_CAP);
  CHECK(!charge(used, 1, 1));
  used = 0;
  CHECK(!charge(used, 10001, 1) && used == 0);
  CHECK(!charge(used, 1, 0));
  CHECK(!charge(used, 10000, 4));
}
static void testKdf() {
  auto work = std::unique_ptr<KdfWork>(new (std::nothrow) KdfWork{});
  CHECK(work != nullptr);
  Backend backend;
  backend.password = "password";
  const Vec salt = {'s', 'a', 'l', 't'};
  std::array<uint8_t, 64> output{};
  CHECK(pbkdf2(backend.callbacks(), *work, Hash::Sha1, view(salt), 1, output.data(), 20));
  CHECK(memcmp(output.data(), hex("0c60c80f961f0e71f3a9b524af6012062fe037a6").data(), 20) == 0);
  CHECK(pbkdf2(backend.callbacks(), *work, Hash::Sha1, view(salt), 2, output.data(), 20));
  CHECK(memcmp(output.data(), hex("ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957").data(), 20) == 0);
  CHECK(pbkdf2(backend.callbacks(), *work, Hash::Sha1, view(salt), 4096, output.data(), 20));
  CHECK(memcmp(output.data(), hex("4b007901b765489abead49d926f721d065a429c1").data(), 20) == 0);
  CHECK(backend.yields >= 128);
  backend.type = Hash::Sha256;
  CHECK(pbkdf2(backend.callbacks(), *work, Hash::Sha256, view(salt), 2, output.data(), 32));
  CHECK(memcmp(output.data(), hex("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43").data(), 32) == 0);
  backend.type = Hash::Sha1;
  const auto p12salt = hex("1234567890abcdef"), unicode = hex("00700061007300730077006f007200640000");
  // Reference values from local OpenSSL's independent PKCS12KDF provider.
  CHECK(pkcs12Kdf(backend.callbacks(), *work, Hash::Sha1, view(p12salt), view(unicode), 2048, 1, output.data(), 24));
  CHECK(memcmp(output.data(), hex("7425f71a07aa5b967a178c4adb4b7a1b90b8697639b05b71").data(), 24) == 0);
  backend.type = Hash::Sha256;
  CHECK(pkcs12Kdf(backend.callbacks(), *work, Hash::Sha256, view(p12salt), view(unicode), 2048, 3, output.data(), 32));
  CHECK(memcmp(output.data(), hex("3b0e9a552e4c9dcdc2b923974199b4113e8daf6c09c2de216e1aded497062276").data(), 32) == 0);
  backend.type = Hash::Sha1;
  const auto empty = hex("0000");
  CHECK(pkcs12Kdf(backend.callbacks(), *work, Hash::Sha1, view(p12salt), view(empty), 1, 2, output.data(), 8));
  CHECK(memcmp(output.data(), hex("fd57efbebb50e8e4").data(), 8) == 0);
  CHECK(!pkcs12Kdf(backend.callbacks(), *work, Hash::Sha1, view(p12salt), view(empty), 10001, 2, output.data(), 8));
  CHECK(!pkcs12Kdf(backend.callbacks(), *work, Hash::Sha1, view(p12salt), view(empty), 1, 0, output.data(), 8));
  CHECK(!pbkdf2(backend.callbacks(), *work, Hash::Sha1, view(salt), 0, output.data(), 20));
  backend.fail = true;
  CHECK(!pbkdf2(backend.callbacks(), *work, Hash::Sha1, view(salt), 2, output.data(), 20));
  CHECK(!pkcs12Kdf(backend.callbacks(), *work, Hash::Sha1, view(p12salt), view(empty), 2, 1, output.data(), 24));
}
static void testLegacyCiphers() {
  // Independent local OpenSSL CBC fixtures; use the same in-place/chunked
  // primitives as the firmware, including continuation IV handling.
  const std::string_view plain = "plugin-crypto CBC vector";
  const auto iv = hex("0102030405060708");
  const auto key = hex("7425f71a07aa5b967a178c4adb4b7a1b90b8697639b05b71");
  Vec data = hex("cad476b6485b6c32d8b651ea71e4bfcb97c4e1fccb816cce4ef942988b5a5da3");
  Des3 des;
  CHECK(wc_Des3Init(&des, nullptr, INVALID_DEVID) == 0);
  CHECK(wc_Des3_SetKey(&des, key.data(), iv.data(), DES_DECRYPTION) == 0);
  for (size_t n = 0; n < data.size(); n += 8) CHECK(wc_Des3_CbcDecrypt(&des, data.data() + n, data.data() + n, 8) == 0);
  wc_Des3Free(&des);
  size_t size = data.size();
  CHECK(unpad(data.data(), size, 8) && size == plain.size() && memcmp(data.data(), plain.data(), size) == 0);
  const auto rcKey = hex("0102030405");
  data = hex("bd7ce5c927ce26440f67c17085c863197a0c88f3912b6bab0170c51b40796d7d");
  Rc2 rc;
  CHECK(wc_Rc2SetKey(&rc, rcKey.data(), rcKey.size(), iv.data(), 40) == 0);
  for (size_t n = 0; n < data.size(); n += 8) CHECK(wc_Rc2CbcDecrypt(&rc, data.data() + n, data.data() + n, 8) == 0);
  size = data.size();
  CHECK(unpad(data.data(), size, 8) && size == plain.size() && memcmp(data.data(), plain.data(), size) == 0);
}
int main() {
  testBase64();
  testDerAndPadding();
  testPbe();
  testKdf();
  testLegacyCiphers();
  std::printf("PluginCryptoPolicyTest: %u checks passed\n", checks);
}
