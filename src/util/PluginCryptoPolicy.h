#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace plugincrypto::policy {
inline constexpr size_t BODY_CAP = 16384;
inline constexpr size_t DECODE_CAP = 8192;  // aggregate decoded request fields
inline constexpr size_t RANDOM_CAP = 4096;
inline constexpr size_t RSA_CAP = 512;
inline constexpr size_t PASSWORD_CAP = 128;  // byte-compatible upstream password
inline constexpr size_t BAG_CAP = 16;
inline constexpr size_t SAFE_CAP = 8;
inline constexpr uint32_t ITERATION_CAP = 10000;
inline constexpr uint32_t KDF_WORK_CAP = 30000;  // digest rounds, whole request

inline int base64Digit(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}
// Canonical, padded RFC 4648 base64; reject whitespace, interior '=', embedded
// NUL and nonzero unused bits rather than silently treating invalid data as empty.
inline bool decodedSize(std::string_view input, size_t& size) {
  size = 0;
  if (input.empty()) return true;
  if (input.size() % 4 || input.size() > 4 * ((DECODE_CAP + 2) / 3)) return false;
  size_t pad = input.back() == '=' ? 1 : 0;
  if (pad && input[input.size() - 2] == '=') ++pad;
  for (size_t i = 0; i < input.size() - pad; ++i)
    if (base64Digit(static_cast<unsigned char>(input[i])) < 0) return false;
  if (pad == 2 && (base64Digit(input[input.size() - 3]) & 15)) return false;
  if (pad == 1 && (base64Digit(input[input.size() - 2]) & 3)) return false;
  size = input.size() / 4 * 3 - pad;
  return size <= DECODE_CAP;
}
inline bool decode(std::string_view input, uint8_t* out, size_t capacity, size_t& size) {
  if (!decodedSize(input, size) || size > capacity || (size && !out)) return false;
  size_t pos = 0;
  for (size_t i = 0; i < input.size(); i += 4) {
    const uint32_t v = (static_cast<uint32_t>(base64Digit(input[i])) << 18) |
                       (static_cast<uint32_t>(base64Digit(input[i + 1])) << 12) |
                       (input[i + 2] == '=' ? 0u : static_cast<uint32_t>(base64Digit(input[i + 2])) << 6) |
                       (input[i + 3] == '=' ? 0u : static_cast<uint32_t>(base64Digit(input[i + 3])));
    if (pos < size) out[pos++] = static_cast<uint8_t>(v >> 16);
    if (pos < size) out[pos++] = static_cast<uint8_t>(v >> 8);
    if (pos < size) out[pos++] = static_cast<uint8_t>(v);
  }
  return true;
}
inline size_t encodedSize(size_t size) { return 4 * ((size + 2) / 3); }
inline bool encode(const uint8_t* input, size_t size, char* out, size_t capacity) {
  if (size > DECODE_CAP + 16 || capacity <= encodedSize(size) || !out || (size && !input)) return false;
  static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t p = 0;
  for (size_t i = 0; i < size; i += 3) {
    uint32_t v = static_cast<uint32_t>(input[i]) << 16;
    if (i + 1 < size) v |= static_cast<uint32_t>(input[i + 1]) << 8;
    if (i + 2 < size) v |= input[i + 2];
    out[p++] = ALPHABET[(v >> 18) & 63];
    out[p++] = ALPHABET[(v >> 12) & 63];
    out[p++] = i + 1 < size ? ALPHABET[(v >> 6) & 63] : '=';
    out[p++] = i + 2 < size ? ALPHABET[v & 63] : '=';
  }
  out[p] = 0;
  return true;
}
inline bool paddedSize(size_t size, size_t& out) {
  if (size > DECODE_CAP) return false;
  out = (size / 16 + 1) * 16;
  return true;
}
inline bool unpad(uint8_t* data, size_t& size, size_t block) {
  if (!data || !size || size % block || (block != 8 && block != 16)) return false;
  const size_t n = data[size - 1];
  if (!n || n > block || n > size) return false;
  unsigned bad = 0;
  for (size_t i = 0; i < n; ++i) bad |= data[size - 1 - i] ^ n;
  if (bad) return false;
  size -= n;
  return true;
}
inline bool signBlock(const uint8_t* hash, size_t hashSize, uint8_t* out, size_t capacity) {
  if (!hash || hashSize != 20 || !out || capacity < 128) return false;
  memset(out, 0xff, 128);
  out[0] = 0;
  out[1] = 1;
  out[107] = 0;
  memcpy(out + 108, hash, 20);
  return true;
}

struct View {
  const uint8_t* data = nullptr;
  size_t size = 0;
};
struct Der {
  View value;
  View encoded;
  uint8_t tag = 0;
};
// Provided by the caller on checked heap; reusable across a request. Keeping
// DER nodes here avoids >256-byte embedded frames without recursive parsing.
struct DerWork {
  Der nodes[12];
  View cursors[4];
};
// Strict definite DER, only low-tag-number objects. No BER indefinite lengths,
// arithmetic wraparound, unaligned loads or unbounded recursive scanner.
inline bool take(View& input, Der& out, int tag = -1) {
  if (!input.data || input.size < 2) return false;
  const uint8_t t = input.data[0];
  if ((t & 31) == 31 || (tag >= 0 && t != tag)) return false;
  size_t h = 2, n = input.data[1];
  if (n & 128) {
    const size_t bytes = n & 127;
    if (!bytes || bytes > 2 || bytes + 2 > input.size || input.data[2] == 0) return false;
    n = 0;
    for (size_t i = 0; i < bytes; ++i) n = n * 256 + input.data[h++];
    if (n < 128) return false;
  }
  if (n > input.size - h) return false;
  out = {{input.data + h, n}, {input.data, h + n}, t};
  input.data += h + n;
  input.size -= h + n;
  return true;
}
inline bool one(View input, Der& out, int tag) { return take(input, out, tag) && input.size == 0; }
inline bool integer(View& input, uint32_t& out) {
  Der d;
  if (!take(input, d, 2) || !d.value.size || d.value.size > 4 || (d.value.data[0] & 128) ||
      (d.value.size > 1 && d.value.data[0] == 0 && !(d.value.data[1] & 128)))
    return false;
  out = 0;
  for (size_t i = 0; i < d.value.size; ++i) out = out * 256 + d.value.data[i];
  return true;
}
inline bool oid(const Der& d, const uint8_t* expected, size_t n) {
  return d.tag == 6 && d.value.size == n && memcmp(d.value.data, expected, n) == 0;
}
inline bool pkcs7Oid(const Der& d, uint8_t suffix) {
  static constexpr uint8_t PREFIX[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 7};
  return d.tag == 6 && d.value.size == 9 && memcmp(d.value.data, PREFIX, 8) == 0 && d.value.data[8] == suffix;
}
inline bool bagOid(const Der& d, uint8_t& suffix) {
  static constexpr uint8_t PREFIX[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 0x0c, 0x0a, 1};
  if (d.tag != 6 || d.value.size != 11 || memcmp(d.value.data, PREFIX, 10)) return false;
  suffix = d.value.data[10];
  return true;
}
inline bool rsaInteger(View& input, size_t& bytes) {
  Der d;
  if (!take(input, d, 2) || !d.value.size || (d.value.data[0] & 128)) return false;
  bytes = d.value.size;
  if (d.value.data[0] == 0) {
    if (bytes == 1 || !(d.value.data[1] & 128)) return false;
    --bytes;
  }
  return bytes <= RSA_CAP;
}
inline bool rsaTraditional(View input) {
  Der root;
  if (!one(input, root, 0x30)) return false;
  View fields = root.value;
  uint32_t version = 0;
  if (!integer(fields, version) || version != 0) return false;
  for (size_t i = 0; i < 8; ++i) {
    size_t bytes = 0;
    if (!rsaInteger(fields, bytes)) return false;
  }
  return fields.size == 0;  // no multi-prime/oversized MPI before backend parse
}
inline bool rsaPrivateDer(View input) {
  Der root;
  if (!one(input, root, 0x30)) return false;
  View fields = root.value;
  uint32_t version = 0;
  if (!integer(fields, version) || version != 0 || !fields.size) return false;
  if (fields.data[0] == 2) return rsaTraditional(input);
  Der algorithm, oidValue, key;
  static constexpr uint8_t RSA[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 1, 1};
  if (!take(fields, algorithm, 0x30) || !take(fields, key, 4) || fields.size) return false;
  View a = algorithm.value;
  if (!take(a, oidValue, 6) || !oid(oidValue, RSA, sizeof(RSA))) return false;
  if (a.size) {
    Der nullValue;
    if (!one(a, nullValue, 5) || nullValue.value.size) return false;
  }
  return rsaTraditional(key.value);
}
// Cap the untrusted certificate's RSA modulus before X509 allocates/validates
// MPIs. Other CA-key algorithms may be parsed by the existing X509 backend.
inline bool certificateRsaCap(View input, bool requireRsa, DerWork& work) {
  auto& root = work.nodes[0];
  auto& tbs = work.nodes[1];
  auto& field = work.nodes[2];
  if (!one(input, root, 0x30)) return false;
  auto& outer = work.cursors[0];
  outer = root.value;
  if (!take(outer, tbs, 0x30)) return false;
  auto& t = work.cursors[1];
  t = tbs.value;
  if (t.size && t.data[0] == 0xa0 && !take(t, field, 0xa0)) return false;
  // serial, signature, issuer, validity, subject, SubjectPublicKeyInfo
  for (size_t n = 0; n < 5; ++n)
    if (!take(t, field, n ? 0x30 : 2)) return false;
  auto& spki = work.nodes[3];
  auto& algorithm = work.nodes[4];
  auto& id = work.nodes[5];
  auto& bits = work.nodes[6];
  auto& pub = work.nodes[7];
  if (!take(t, spki, 0x30)) return false;
  auto& s = work.cursors[2];
  s = spki.value;
  if (!take(s, algorithm, 0x30) || !take(s, bits, 3) || s.size || !bits.value.size || bits.value.data[0]) return false;
  auto& a = work.cursors[3];
  a = algorithm.value;
  static constexpr uint8_t RSA[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 1, 1};
  if (!take(a, id, 6)) return false;
  if (!oid(id, RSA, sizeof(RSA))) return !requireRsa && bits.value.size <= RSA_CAP + 1;
  if (!one({bits.value.data + 1, bits.value.size - 1}, pub, 0x30)) return false;
  outer = pub.value;
  size_t n = 0, e = 0;
  return rsaInteger(outer, n) && rsaInteger(outer, e) && e <= 4 && outer.size == 0;
}
enum class Hash { Sha1, Sha256, Sha384, Sha512 };
inline size_t hashSize(Hash hash) {
  return hash == Hash::Sha1 ? 20 : hash == Hash::Sha256 ? 32 : hash == Hash::Sha384 ? 48 : 64;
}
inline size_t hashBlock(Hash hash) { return hash == Hash::Sha1 || hash == Hash::Sha256 ? 64 : 128; }
inline bool hashOid(const Der& oidValue, Hash& hash, bool hmac = false) {
  static constexpr uint8_t SHA1[] = {0x2b, 0x0e, 3, 2, 0x1a};
  static constexpr uint8_t SHA2[] = {0x60, 0x86, 0x48, 1, 0x65, 3, 4, 2};
  static constexpr uint8_t HMAC[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 2};
  if (!hmac && oid(oidValue, SHA1, sizeof(SHA1))) {
    hash = Hash::Sha1;
    return true;
  }
  const auto& prefix = hmac ? HMAC : SHA2;
  if (oidValue.tag != 6 || oidValue.value.size != (hmac ? 8u : 9u) || memcmp(oidValue.value.data, prefix, hmac ? 7 : 8))
    return false;
  const uint8_t n = oidValue.value.data[oidValue.value.size - 1];
  if (hmac && n == 7) {
    hash = Hash::Sha1;
    return true;
  }
  if (n == (hmac ? 9 : 1))
    hash = Hash::Sha256;
  else if (n == (hmac ? 10 : 2))
    hash = Hash::Sha384;
  else if (n == (hmac ? 11 : 3))
    hash = Hash::Sha512;
  else
    return false;
  return true;
}
enum class Cipher { Aes, Des3, Rc2 };
struct Pbe {
  View salt, iv;
  Hash hash = Hash::Sha1;
  Cipher cipher = Cipher::Aes;
  uint32_t iterations = 0;
  size_t keySize = 0;
  size_t block = 0;
  bool pkcs12 = false;
};
inline bool nullParameters(View input) {
  if (!input.size) return true;
  Der d;
  return one(input, d, 5) && !d.value.size;
}
inline bool pbeParameters(View algorithm, Pbe& out, DerWork& work) {
  auto& seq = work.nodes[0];
  auto& id = work.nodes[1];
  auto& params = work.nodes[2];
  if (!one(algorithm, seq, 0x30)) return false;
  auto& a = work.cursors[0];
  a = seq.value;
  if (!take(a, id, 6) || !take(a, params, 0x30) || a.size) return false;
  static constexpr uint8_t P12[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 0x0c, 1};
  static constexpr uint8_t PBES2[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 5, 0x0d};
  static constexpr uint8_t PBKDF2[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 5, 0x0c};
  static constexpr uint8_t AES[] = {0x60, 0x86, 0x48, 1, 0x65, 3, 4, 1};
  static constexpr uint8_t DES3[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 3, 7};
  auto& salt = work.nodes[3];
  if (id.value.size == 10 && memcmp(id.value.data, P12, 9) == 0) {
    out.pkcs12 = true;
    out.hash = Hash::Sha1;
    const uint8_t type = id.value.data[9];
    if (type == 3 || type == 4) {
      out.cipher = Cipher::Des3;
      out.keySize = type == 3 ? 24 : 16;
    } else if (type == 5 || type == 6) {
      out.cipher = Cipher::Rc2;
      out.keySize = type == 5 ? 16 : 5;
    } else
      return false;  // upstream-rare RC4/unknown algorithms fail explicitly
    out.block = 8;
    auto& p = work.cursors[1];
    p = params.value;
    if (!take(p, salt, 4) || !integer(p, out.iterations) || p.size) return false;
  } else {
    if (!oid(id, PBES2, sizeof(PBES2))) return false;
    out.pkcs12 = false;
    auto& p = work.cursors[1];
    p = params.value;
    auto& kdf = work.nodes[4];
    auto& enc = work.nodes[5];
    auto& kdfId = work.nodes[6];
    auto& kdfParams = work.nodes[7];
    auto& encId = work.nodes[8];
    auto& iv = work.nodes[9];
    if (!take(p, kdf, 0x30) || !take(p, enc, 0x30) || p.size) return false;
    auto& k = work.cursors[2];
    k = kdf.value;
    if (!take(k, kdfId, 6) || !oid(kdfId, PBKDF2, sizeof(PBKDF2)) || !take(k, kdfParams, 0x30) || k.size) return false;
    k = kdfParams.value;
    if (!take(k, salt, 4) || !integer(k, out.iterations)) return false;
    uint32_t requestedKey = 0;
    if (k.size && k.data[0] == 2 && !integer(k, requestedKey)) return false;
    out.hash = Hash::Sha1;
    if (k.size) {
      auto& prf = work.nodes[10];
      auto& prfId = work.nodes[11];
      if (!take(k, prf, 0x30) || k.size) return false;
      auto& f = work.cursors[3];
      f = prf.value;
      if (!take(f, prfId, 6) || !hashOid(prfId, out.hash, true) || !nullParameters(f)) return false;
    }
    auto& e = work.cursors[3];
    e = enc.value;
    if (!take(e, encId, 6) || !take(e, iv, 4) || e.size) return false;
    if (encId.value.size == 9 && memcmp(encId.value.data, AES, 8) == 0) {
      const uint8_t n = encId.value.data[8];
      out.keySize = n == 2 ? 16 : n == 22 ? 24 : n == 42 ? 32 : 0;
      out.cipher = Cipher::Aes;
      out.block = 16;
    } else if (oid(encId, DES3, sizeof(DES3))) {
      out.cipher = Cipher::Des3;
      out.keySize = 24;
      out.block = 8;
    } else
      return false;
    if (!out.keySize || (requestedKey && requestedKey != out.keySize) || iv.value.size != out.block) return false;
    out.iv = iv.value;
  }
  out.salt = salt.value;
  return out.salt.size && out.salt.size <= 64 && out.iterations && out.iterations <= ITERATION_CAP;
}
inline bool charge(uint32_t& used, uint32_t iterations, size_t blocks) {
  if (!iterations || iterations > ITERATION_CAP || !blocks || blocks > KDF_WORK_CAP / iterations) return false;
  const uint32_t amount = iterations * blocks;
  if (used > KDF_WORK_CAP - amount) return false;
  used += amount;
  return true;
}

// Caller-supplied heap workspace: no large stack buffers and no allocations in
// the KDF loops. Digest/HMAC callbacks return false on every backend error.
struct KdfWork {
  uint8_t i[512], d[128], a[64], b[128], u[64], t[64];
};
struct KdfCallbacks {
  void* context;
  bool (*digest)(void*, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*);
  bool (*hmac)(void*, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*);
  void (*cooperate)(void*);
};
inline void cooperate(const KdfCallbacks& cb, uint32_t n) {
  if (cb.cooperate && (n & 31) == 0) cb.cooperate(cb.context);
}
inline bool pbkdf2(const KdfCallbacks& cb, KdfWork& w, Hash hash, View salt, uint32_t iterations, uint8_t* out,
                   size_t size) {
  const size_t u = hashSize(hash);
  if (!cb.hmac || !out || !size || size > 64 || !salt.size || salt.size > 64 || !iterations ||
      iterations > ITERATION_CAP)
    return false;
  for (uint32_t block = 1; size; ++block) {
    const uint8_t index[] = {static_cast<uint8_t>(block >> 24), static_cast<uint8_t>(block >> 16),
                             static_cast<uint8_t>(block >> 8), static_cast<uint8_t>(block)};
    if (!cb.hmac(cb.context, salt.data, salt.size, index, 4, w.u)) return false;
    memcpy(w.t, w.u, u);
    for (uint32_t n = 1; n < iterations; ++n) {
      if (!cb.hmac(cb.context, w.u, u, nullptr, 0, w.u)) return false;
      for (size_t j = 0; j < u; ++j) w.t[j] ^= w.u[j];
      cooperate(cb, n);
    }
    const size_t n = size < u ? size : u;
    memcpy(out, w.t, n);
    out += n;
    size -= n;
    if (cb.cooperate) cb.cooperate(cb.context);
  }
  return true;
}
inline bool pkcs12Kdf(const KdfCallbacks& cb, KdfWork& w, Hash hash, View salt, View unicodePassword,
                      uint32_t iterations, uint8_t id, uint8_t* out, size_t size) {
  const size_t u = hashSize(hash), v = hashBlock(hash);
  if (!cb.digest || !out || !size || size > 64 || salt.size > 64 || unicodePassword.size > 258 || !iterations ||
      iterations > ITERATION_CAP || id < 1 || id > 3)
    return false;
  const size_t s = salt.size ? v * ((salt.size + v - 1) / v) : 0;
  const size_t p = unicodePassword.size ? v * ((unicodePassword.size + v - 1) / v) : 0;
  if (s + p > sizeof(w.i)) return false;
  memset(w.d, id, v);
  for (size_t j = 0; j < s; ++j) w.i[j] = salt.data[j % salt.size];
  for (size_t j = 0; j < p; ++j) w.i[s + j] = unicodePassword.data[j % unicodePassword.size];
  while (size) {
    if (!cb.digest(cb.context, w.d, v, w.i, s + p, w.a)) return false;
    for (uint32_t n = 1; n < iterations; ++n) {
      if (!cb.digest(cb.context, w.a, u, nullptr, 0, w.a)) return false;
      cooperate(cb, n);
    }
    const size_t n = size < u ? size : u;
    memcpy(out, w.a, n);
    out += n;
    size -= n;
    if (size) {
      for (size_t j = 0; j < v; ++j) w.b[j] = w.a[j % u];
      for (size_t offset = 0; offset < s + p; offset += v) {
        unsigned carry = 1;
        for (size_t j = v; j; --j) {
          carry += w.i[offset + j - 1] + w.b[j - 1];
          w.i[offset + j - 1] = static_cast<uint8_t>(carry);
          carry >>= 8;
        }
      }
    }
    if (cb.cooperate) cb.cooperate(cb.context);
  }
  return true;
}
}  // namespace plugincrypto::policy
