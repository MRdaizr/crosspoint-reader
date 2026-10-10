#pragma once
#include <cstddef>
#include <cstdint>

#include "PluginEventsPolicy.h"
class GfxRenderer;
namespace pluginevents {
enum class Event : uint8_t { ReaderOpen, ReaderExit, ReaderSession, BookDownloaded, SleepEnter, COUNT };
struct Var {
  const char* key;
  const char* value;
};
void refreshSubscriptions();
bool anySubscriber(Event event);
uint8_t subscriptionMask(const char* plugin);
bool wantsConnectAny();
bool shouldConnectForSleep(bool pluginSleepConnect, int batteryPercent, bool hasSavedNetwork);
void emit(Event event, const Var* vars, size_t count);
void drain(GfxRenderer* renderer, size_t maxEvents = 4, uint32_t deadlineMs = 0, bool (*cancel)(void*) = nullptr,
           void* context = nullptr);
}  // namespace pluginevents
