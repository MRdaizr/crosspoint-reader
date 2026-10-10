#pragma once
#include <cstdint>
#include <mutex>
using TickType_t = uint32_t;
using TaskHandle_t = void*;
// A pointer keeps the production static-budget check meaningful on a 64-bit host.
using portMUX_TYPE = std::recursive_mutex*;
#define portMUX_INITIALIZER_UNLOCKED nullptr
void fakeEnterCritical(portMUX_TYPE*);
void fakeExitCritical(portMUX_TYPE*);
#define portENTER_CRITICAL(lock) fakeEnterCritical(lock)
#define portEXIT_CRITICAL(lock) fakeExitCritical(lock)
inline constexpr TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
