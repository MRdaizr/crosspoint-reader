#pragma once
#include <lwip/ip_addr.h>
class IPAddress {
 public:
  IPAddress() = default;
  explicit IPAddress(uint32_t value) { *this = value; }
  IPAddress& operator=(uint32_t value) {
    address = {};
    for (unsigned i = 0; i < 4; ++i) address.bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    return *this;
  }
  bool fromString(const char* text);
  IPAddress& from_ip_addr_t(const ip_addr_t* value) { address = *value; return *this; }
  ip_addr_t address{};
};
