#include <wolfssl/ssl.h>

#include <cstring>

// weread.qq.com intermittently closes the connection when the ClientHello
// advertises a large record size.  Keep the workaround isolated to that SNI
// host; all other wolfSSL users retain the normal negotiated fragment size.
extern "C" {
int __real_wolfSSL_UseSNI(WOLFSSL* ssl, unsigned char type, const void* data, unsigned short size);

int __wrap_wolfSSL_UseSNI(WOLFSSL* ssl, unsigned char type, const void* data, unsigned short size) {
  const int result = __real_wolfSSL_UseSNI(ssl, type, data, size);
  static constexpr char kWeReadHost[] = "weread.qq.com";
  if (result == WOLFSSL_SUCCESS && ssl != nullptr && data != nullptr && type == WOLFSSL_SNI_HOST_NAME &&
      size == sizeof(kWeReadHost) - 1 && std::memcmp(data, kWeReadHost, sizeof(kWeReadHost) - 1) == 0) {
    wolfSSL_UseMaxFragment(ssl, WOLFSSL_MFL_2_12);
  }
  return result;
}
}
