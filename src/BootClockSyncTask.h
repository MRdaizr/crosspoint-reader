#pragma once

namespace BootClockSyncTask {

// Starts the one-shot background clock sync. The task does not block the UI
// loop and silently exits when there is no saved network or the clock is
// already valid.
void start();

// Request cancellation before sleep or another foreground network operation.
void cancel();

}  // namespace BootClockSyncTask
