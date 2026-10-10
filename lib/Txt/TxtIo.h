#pragma once
#include "TxtToHtml.h"
#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../../src/util/TaskWatchdog.h"
#endif

namespace TxtIo {
inline bool cancelled(const TxtToHtml::Callbacks* cb) { return cb && cb->cancelled && cb->cancelled(cb->context); }
// Library lookups also run while a reader saves progress, without a preparation
// callback. Always cooperate on device; host tests have no RTOS dependency.
inline bool pulse(const TxtToHtml::Callbacks* cb, uint32_t bytes, uint32_t total) {
  if (cancelled(cb)) return false;
#if defined(ARDUINO)
  resetTaskWatchdogIfSubscribed();
#endif
  if (cb && cb->progress) cb->progress(cb->context, bytes, total);
#if defined(ARDUINO)
  else
    vTaskDelay(1);
#endif
  return !cancelled(cb);
}
}  // namespace TxtIo
