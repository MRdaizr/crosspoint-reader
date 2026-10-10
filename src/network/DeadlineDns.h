#pragma once

#include <cstdint>

// Only tasks with a live Guard use bounded DNS. deadlineMs is an ABSOLUTE
// millis() deadline (including zero at wrap), less than 2^31 ms ahead.
// No task is created. Guards must leave scope before their task is deleted;
// neither a live Guard nor its cancellation context may be destroyed remotely.
namespace DeadlineDns {
struct Access;

class Guard {
 public:
  using CancelCheck = bool (*)(void* context);

  explicit Guard(uint32_t deadlineMs, CancelCheck shouldCancel = nullptr, void* context = nullptr);
  ~Guard();
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
  Guard(Guard&&) = delete;
  Guard& operator=(Guard&&) = delete;

  // May be called from another TASK while the Guard remains alive, not an ISR.
  // CancelCheck instead runs on the guarded task outside any critical section;
  // it must be quick/nonblocking and its context must outlive this Guard.
  void cancel();

 private:
  friend struct Access;
  void* task;
  Guard* next;
  Guard* parent;
  uint32_t deadline;
  CancelCheck shouldCancel;
  void* context;
  bool cancelled = false;
};
}  // namespace DeadlineDns

// Parent integration: -Wl,--wrap=_ZN14NetworkManager10hostByNameEPKcR9IPAddress
// DNS requests use four static slots, hostname <=253 bytes. Queue/pool exhaustion
// fails closed; timed-out slots remain reserved until their TCP/IP/DNS callback.
// The wait quantum is 5ms, rounded to at least one FreeRTOS tick. No core-lock
// wait, DNS cache mutation, NTP operation, or SDK/submodule modification occurs.
