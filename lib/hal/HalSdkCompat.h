#pragma once

// Keep the rest of the firmware insulated from the SDK package rename and
// small API spelling differences between open-x4-sdk and freeink-sdk.  The
// Application code talks to the Hal* classes; only HAL and network adapter
// implementations should include SDK headers directly.

#include <EInkDisplay.h>

namespace hal_sdk {

using Display = EInkDisplay;

}  // namespace hal_sdk
