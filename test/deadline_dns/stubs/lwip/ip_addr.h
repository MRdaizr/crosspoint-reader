#pragma once
#include <array>
#include <cstdint>
struct ip_addr_t {
  std::array<uint8_t, 16> bytes{};
  uint8_t type = 4;
  uint8_t zone = 0;
  bool operator==(const ip_addr_t&) const = default;
};
