#pragma once
// ESP-only helper is a no-op on host; production builder/query's ARDUINO guard
// remains disabled. The actual source file is not copied or edited.
inline void resetTaskWatchdogIfSubscribed() {}
