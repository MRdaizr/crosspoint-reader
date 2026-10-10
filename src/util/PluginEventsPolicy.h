#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace pluginevents {
inline constexpr size_t MAX_OUTBOX_BYTES = 4096, MAX_EVENT_LINE = 512, MAX_DRAIN_EVENTS = 4;
inline constexpr uint32_t CONNECT_BUDGET_MS = 10000, SLEEP_BUDGET_MS = 20000;
inline bool withinDeadline(uint32_t now, uint32_t deadline) {
  return deadline == 0 || static_cast<int32_t>(now - deadline) < 0;
}
inline bool sleepConnectAllowed(bool enabled, int battery, bool savedNetwork, bool queued) {
  return enabled && battery >= 20 && savedNetwork && queued;
}
struct DrainPrefix {
  size_t bytes = 0, attempts = 0;
  bool stalled = false;
};
// Last complete line boundary. Oversized buffers are rejected rather than
// scanned without a bound; a completely torn/corrupt tail has length zero.
inline size_t recoverAppendLength(const char* data, size_t size) {
  if (size > MAX_OUTBOX_BYTES || (!data && size)) return SIZE_MAX;
  for (size_t pos = size; pos; --pos)
    if (data[pos - 1] == '\n') return pos;
  return 0;
}
// Pure outbox transaction: consume only an acknowledged prefix. Retries see
// the original JSON line and id unchanged; a failure stops that plugin's order.
inline DrainPrefix drainPrefix(const char* data, size_t size, size_t budget,
                               bool (*deliver)(void*, const char*, size_t), void* context,
                               bool (*cancel)(void*) = nullptr) {
  DrainPrefix result;
  budget = std::min(budget, MAX_DRAIN_EVENTS);
  while (result.bytes < size && result.attempts < budget) {
    if (cancel && cancel(context)) break;
    const char* first = data + result.bytes;
    const char* nl = static_cast<const char*>(memchr(first, '\n', size - result.bytes));
    if (!nl) {
      result.stalled = true;
      break;
    }  // preserve a torn tail
    const size_t length = nl - first;
    if (length) {
      ++result.attempts;
      if (length + 1 > MAX_EVENT_LINE || !deliver(context, first, length)) {
        result.stalled = true;
        break;
      }
    }
    result.bytes += length + 1;
  }
  return result;
}
}  // namespace pluginevents
