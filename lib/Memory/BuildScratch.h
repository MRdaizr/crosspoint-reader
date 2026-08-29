#pragma once

#include <cstddef>
#include <cstdint>

// Registry for the framebuffer storage temporarily lent to memory-hungry
// conversion/build phases.  The lender (GfxRenderer) owns the lifetime;
// consumers claim the block for the duration of one operation and release it
// before the renderer restores the framebuffer.
namespace buildscratch {

void lend(uint8_t* buffer, size_t length);
void reclaim();

// Claim the entire lent block when it is at least minLength bytes.  Returns
// nullptr when no loan is active, the block is too small, or another consumer
// already owns it.  lenOut receives the actual block size when non-null.
uint8_t* claim(size_t minLength, size_t* lenOut = nullptr);
void release(const uint8_t* buffer);

}  // namespace buildscratch
