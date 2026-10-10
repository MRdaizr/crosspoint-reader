#pragma once
#include <IPAddress.h>
class NetworkManager {
 public:
  int hostByName(const char* host, IPAddress& result);
};
