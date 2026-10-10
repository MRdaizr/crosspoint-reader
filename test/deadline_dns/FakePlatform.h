#pragma once
#include <NetworkManager.h>
#include <freertos/FreeRTOS.h>
#include <lwip/tcpip.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace fake {
struct Post {
  tcpip_callback_fn callback;
  void* argument;
};
struct Pending {
  dns_found_callback callback;
  void* argument;
  std::string host;
};
extern uint64_t now;
extern void* task;
extern unsigned criticalDepth;
extern bool inTcpip;
extern bool autoStart;
extern err_t enqueueError;
extern err_t dnsError;
extern ip_addr_t answer;
extern std::function<void()> onDelay;
extern std::deque<Post> posts;
extern std::deque<Pending> pending;
extern std::vector<std::string> dnsHosts;
extern std::vector<uint32_t> waits;
extern unsigned realCalls;
extern NetworkManager* realManager;
extern const char* realHost;
extern int realReturn;
void reset();
void startOne();
void deliverOne(const ip_addr_t* address);
void drain();
void* taskId(uintptr_t id);
ip_addr_t ipv4(uint8_t last = 42);
ip_addr_t ipv6(uint8_t zone = 7);
}  // namespace fake
