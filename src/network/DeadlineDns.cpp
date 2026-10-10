#include "DeadlineDns.h"

#include <IPAddress.h>
#include <Logging.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>

#include <cstring>

namespace {
constexpr size_t SLOT_COUNT = 4;
constexpr size_t MAX_HOST_LENGTH = 253;
enum class State : uint8_t { Free, Queued, Resolving, Complete };

struct Slot {
  ip_addr_t address{};
  uint32_t deadline = 0;
  char host[MAX_HOST_LENGTH + 1]{};
  State state = State::Free;
  bool attached = false;
  bool success = false;
};

// Persistent callback arguments: never a Guard, caller stack, or task pointer.
// There is no request heap allocation, including the copied hostname.
Slot slots[SLOT_COUNT];
DeadlineDns::Guard* guards = nullptr;
portMUX_TYPE poolLock = portMUX_INITIALIZER_UNLOCKED;
static_assert(sizeof(slots) + sizeof(guards) + sizeof(poolLock) <= 1536,
              "DeadlineDns must fit the 1.5KiB static DRAM budget");

class Lock {
 public:
  Lock() { portENTER_CRITICAL(&poolLock); }
  ~Lock() { portEXIT_CRITICAL(&poolLock); }
};

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

uint32_t remainingMs(uint32_t now, uint32_t deadline) {
  const uint32_t delta = deadline - now;
  return delta < 0x80000000u ? delta : 0;
}

void release(Slot& slot) {
  slot.attached = false;
  slot.state = State::Free;
}

void finish(Slot& slot, const ip_addr_t* address) {
  Lock lock;
  if (!slot.attached) {
    release(slot);
    return;
  }
  slot.success = address != nullptr;
  if (address) slot.address = *address;
  slot.state = State::Complete;
}

void dnsFound(const char*, const ip_addr_t* address, void* context) { finish(*static_cast<Slot*>(context), address); }

// This is the ONLY caller of the raw DNS API, on the lwIP TCP/IP thread.
void startDns(void* context) {
  auto& slot = *static_cast<Slot*>(context);
  const uint32_t now = nowMs();
  bool expired = false;
  {
    Lock lock;
    if (!slot.attached) {
      release(slot);  // Timeout/cancel while still waiting in the TCP/IP mailbox.
      return;
    }
    expired = remainingMs(now, slot.deadline) == 0;
    slot.state = State::Resolving;
  }
  if (expired) {
    finish(slot, nullptr);
    return;
  }
  ip_addr_t address{};
  const err_t error = dns_gethostbyname(slot.host, &address, dnsFound, &slot);
  // Per lwIP contract, only ERR_INPROGRESS installs a future callback.
  if (error != ERR_INPROGRESS) finish(slot, error == ERR_OK ? &address : nullptr);
}

Slot* acquire(const char* host, size_t length, uint32_t deadline) {
  Lock lock;
  for (auto& slot : slots) {
    if (slot.state != State::Free) continue;
    std::memcpy(slot.host, host, length);
    slot.host[length] = '\0';
    slot.deadline = deadline;
    slot.attached = true;
    slot.success = false;
    slot.state = State::Queued;
    return &slot;
  }
  return nullptr;
}

void detach(Slot& slot) {
  Lock lock;
  slot.attached = false;
  if (slot.state == State::Complete) release(slot);
  // Queued/Resolving ownership ends only at startDns/dnsFound, not on timeout.
}
}  // namespace

namespace DeadlineDns {
struct Access {
  struct Policy {
    Guard* head = nullptr;
    uint32_t remaining = 0x7fffffffu;
    bool stopped = false;
  };

  static Policy policy(void* task, uint32_t now) {
    Policy result;
    {
      Lock lock;
      for (auto* guard = guards; guard; guard = guard->next) {
        if (guard->task != task) continue;
        if (!result.head) result.head = guard;
        const auto remaining = remainingMs(now, guard->deadline);
        if (remaining < result.remaining) result.remaining = remaining;
        result.stopped = result.stopped || guard->cancelled || remaining == 0;
      }
    }
    // Same-task ancestors cannot leave scope while this task is in hostByName.
    // Foreign tasks may only cancel(), not destroy/move a live Guard.
    for (auto* guard = result.head; guard && !result.stopped; guard = guard->parent) {
      if (guard->shouldCancel && guard->shouldCancel(guard->context)) result.stopped = true;
    }
    return result;
  }
};

Guard::Guard(uint32_t deadlineMs, CancelCheck check, void* ctx)
    : task(xTaskGetCurrentTaskHandle()),
      next(nullptr),
      parent(nullptr),
      deadline(deadlineMs),
      shouldCancel(check),
      context(ctx) {
  Lock lock;
  for (auto* guard = guards; guard; guard = guard->next) {
    if (guard->task == task) {
      parent = guard;
      break;
    }
  }
  next = guards;
  guards = this;
}

Guard::~Guard() {
  Lock lock;
  for (auto** entry = &guards; *entry; entry = &(*entry)->next) {
    if (*entry == this) {
      *entry = next;
      break;
    }
  }
  for (auto* guard = guards; guard; guard = guard->next) {
    if (guard->parent == this) guard->parent = parent;
  }
}

void Guard::cancel() {
  Lock lock;
  cancelled = true;
}
}  // namespace DeadlineDns

// A member-function ABI has a hidden `this` first argument. Keep the SDK's
// exact Itanium symbol and int return type; do not substitute WiFi::hostByName.
class NetworkManager;
extern "C" int __real__ZN14NetworkManager10hostByNameEPKcR9IPAddress(NetworkManager*, const char*, IPAddress&);
extern "C" int __wrap__ZN14NetworkManager10hostByNameEPKcR9IPAddress(NetworkManager* manager, const char* host,
                                                                     IPAddress& result) {
  void* task = xTaskGetCurrentTaskHandle();
  auto policy = DeadlineDns::Access::policy(task, nowMs());
  if (!policy.head) return __real__ZN14NetworkManager10hostByNameEPKcR9IPAddress(manager, host, result);

  result = static_cast<uint32_t>(0);
  if (policy.stopped || !host) return 0;
  size_t length = 0;
  while (length <= MAX_HOST_LENGTH && host[length]) ++length;
  if (!length || length > MAX_HOST_LENGTH) {
    LOG_ERR("DNS", "Invalid or oversized guarded hostname");
    return 0;
  }
  IPAddress literal;
  if (literal.fromString(host)) {
    if (DeadlineDns::Access::policy(task, nowMs()).stopped) return 0;
    result = literal;
    return 1;
  }

  const uint32_t now = nowMs();
  policy = DeadlineDns::Access::policy(task, now);
  if (policy.stopped) return 0;
  auto* slot = acquire(host, length, now + policy.remaining);
  if (!slot) {
    LOG_ERR("DNS", "Guarded DNS slot pool exhausted");
    return 0;
  }
  // Nonblocking post: ERR_MEM/queue saturation fails closed, never SDK fallback.
  if (tcpip_try_callback(startDns, slot) != ERR_OK) {
    {
      Lock lock;
      release(*slot);
    }
    LOG_ERR("DNS", "Unable to queue guarded DNS");
    return 0;
  }
  for (;;) {
    policy = DeadlineDns::Access::policy(task, nowMs());
    if (policy.stopped) {
      detach(*slot);
      return 0;
    }
    ip_addr_t address{};
    bool complete = false;
    bool success = false;
    {
      Lock lock;
      if (slot->state == State::Complete) {
        complete = true;
        success = slot->success;
        if (success) address = slot->address;
        release(*slot);
      }
    }
    if (complete) {
      if (DeadlineDns::Access::policy(task, nowMs()).stopped) return 0;
      // This conversion preserves IPv4/IPv6 family and IPv6 scope zone.
      if (success) result.from_ip_addr_t(&address);
      return success ? 1 : 0;
    }
    const uint32_t waitMs = policy.remaining < 5 ? policy.remaining : 5;
    const TickType_t ticks = pdMS_TO_TICKS(waitMs);
    vTaskDelay(ticks ? ticks : 1);
  }
}
