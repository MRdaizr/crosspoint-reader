#include "FakePlatform.h"

#include <arpa/inet.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <gtest/gtest.h>

namespace fake {
uint64_t now = 0;
void* task = taskId(1);
unsigned criticalDepth = 0;
bool inTcpip = false;
bool autoStart = true;
err_t enqueueError = ERR_OK;
err_t dnsError = ERR_INPROGRESS;
ip_addr_t answer = ipv4();
std::function<void()> onDelay;
std::deque<Post> posts;
std::deque<Pending> pending;
std::vector<std::string> dnsHosts;
std::vector<uint32_t> waits;
unsigned realCalls = 0;
NetworkManager* realManager = nullptr;
const char* realHost = nullptr;
int realReturn = 17;
std::recursive_mutex mutex;

void* taskId(uintptr_t id) { return reinterpret_cast<void*>(id); }
ip_addr_t ipv4(uint8_t last) {
  ip_addr_t address;
  address.bytes[0] = 192;
  address.bytes[1] = 0;
  address.bytes[2] = 2;
  address.bytes[3] = last;
  return address;
}
ip_addr_t ipv6(uint8_t zone) {
  ip_addr_t address;
  address.type = 6;
  address.zone = zone;
  address.bytes[0] = 0xfe;
  address.bytes[1] = 0x80;
  address.bytes[15] = 1;
  return address;
}
void startOne() {
  ASSERT_FALSE(posts.empty());
  const auto post = posts.front();
  posts.pop_front();
  EXPECT_EQ(criticalDepth, 0u);
  inTcpip = true;
  post.callback(post.argument);
  inTcpip = false;
}
void deliverOne(const ip_addr_t* address) {
  ASSERT_FALSE(pending.empty());
  auto request = pending.front();
  pending.pop_front();
  EXPECT_EQ(criticalDepth, 0u);
  inTcpip = true;
  request.callback(request.host.c_str(), address, request.argument);
  inTcpip = false;
}
void drain() {
  while (!posts.empty()) startOne();
  while (!pending.empty()) deliverOne(nullptr);
}
void reset() {
  drain();
  now = 0;
  task = taskId(1);
  autoStart = true;
  enqueueError = ERR_OK;
  dnsError = ERR_INPROGRESS;
  answer = ipv4();
  onDelay = {};
  dnsHosts.clear();
  waits.clear();
  realCalls = 0;
  realManager = nullptr;
  realHost = nullptr;
  realReturn = 17;
  EXPECT_EQ(criticalDepth, 0u);
}
}  // namespace fake

void fakeEnterCritical(portMUX_TYPE*) {
  fake::mutex.lock();
  ++fake::criticalDepth;
}
void fakeExitCritical(portMUX_TYPE*) {
  --fake::criticalDepth;
  fake::mutex.unlock();
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return fake::task; }
int64_t esp_timer_get_time() { return static_cast<int64_t>(fake::now * 1000); }
void vTaskDelay(TickType_t ticks) {
  EXPECT_EQ(fake::criticalDepth, 0u);
  EXPECT_FALSE(fake::inTcpip);
  fake::waits.push_back(ticks);
  fake::now += ticks;
  if (fake::autoStart && !fake::posts.empty()) fake::startOne();
  if (fake::onDelay) fake::onDelay();
}
err_t tcpip_try_callback(tcpip_callback_fn callback, void* argument) {
  EXPECT_EQ(fake::criticalDepth, 0u);
  if (fake::enqueueError != ERR_OK) return fake::enqueueError;
  fake::posts.push_back({callback, argument});
  return ERR_OK;
}
err_t dns_gethostbyname(const char* host, ip_addr_t* address, dns_found_callback found, void* argument) {
  EXPECT_TRUE(fake::inTcpip);
  EXPECT_EQ(fake::criticalDepth, 0u);
  fake::dnsHosts.emplace_back(host);
  if (fake::dnsError == ERR_INPROGRESS) fake::pending.push_back({found, argument, host});
  if (fake::dnsError == ERR_OK) *address = fake::answer;
  return fake::dnsError;
}
bool IPAddress::fromString(const char* text) {
  ip_addr_t parsed;
  if (inet_pton(AF_INET, text, parsed.bytes.data()) == 1) {
    address = parsed;
    return true;
  }
  if (inet_pton(AF_INET6, text, parsed.bytes.data()) == 1) {
    parsed.type = 6;
    address = parsed;
    return true;
  }
  return false;
}
int NetworkManager::hostByName(const char* host, IPAddress& result) {
  ++fake::realCalls;
  fake::realManager = this;
  fake::realHost = host;
  result.from_ip_addr_t(&fake::answer);
  return fake::realReturn;
}
