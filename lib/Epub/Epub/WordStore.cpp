#include "WordStore.h"

#include <Logging.h>

bool WordStore::ensureChunkSlot() {
  if (chunkCount_ < chunkCapacity_) return true;
  // Manual nothrow growth: std::vector would abort on OOM under -fno-exceptions.
  const size_t newCapacity = chunkCapacity_ == 0 ? 8 : chunkCapacity_ * 2;
  auto grown = makeUniqueNoThrow<Chunk[]>(newCapacity);
  if (!grown) return false;
  for (size_t i = 0; i < chunkCount_; i++) {
    grown[i] = std::move(chunks_[i]);
  }
  chunks_ = std::move(grown);
  chunkCapacity_ = newCapacity;
  return true;
}

bool WordStore::append(const char* text, size_t len, StoredWord& out) {
  // Reject rather than truncate in the middle of a UTF-8 codepoint.
  if (len > MAX_WORD_BYTES) return false;
  const size_t need = len + 1;

  Chunk* target = nullptr;
  if (chunkCount_ > 0) {
    Chunk& last = chunks_[chunkCount_ - 1];
    // Soft flushes may drain the tail and then append more of the same
    // paragraph. Reuse its arena instead of keeping an empty retired tail.
    if (last.live == 0) last.used = 0;
    if (last.data && static_cast<size_t>(last.capacity) - last.used >= need)
      target = &last;
    else if (last.live == 0)
      last.data.reset();
  }
  if (!target) {
    if (!ensureChunkSlot()) return false;
    // Words larger than a chunk get a dedicated exact-fit chunk so the offset
    // arithmetic stays uniform; everything else shares 2KB chunks.
    const size_t cap = need > CHUNK_SIZE ? need : CHUNK_SIZE;
    auto data = makeUniqueNoThrow<char[]>(cap);
    if (!data) return false;
    Chunk& fresh = chunks_[chunkCount_];
    fresh.data = std::move(data);
    fresh.capacity = static_cast<uint16_t>(cap);
    fresh.used = 0;
    fresh.live = 0;
    chunkCount_++;
    target = &fresh;
  }

  // Typed locals: unique_ptr<T[]>::get() is T*, but cppcheck reads the
  // array-form template's get() as void* and flags the arithmetic below.
  const Chunk* chunkBase = chunks_.get();
  char* dst = target->data.get();
  out.chunk = static_cast<uint32_t>(target - chunkBase);
  out.off = target->used;
  out.len = static_cast<uint16_t>(len);
  memcpy(dst + target->used, text, len);
  target->data[target->used + len] = '\0';
  target->used = static_cast<uint16_t>(target->used + need);
  target->live++;
  return true;
}

void WordStore::release(const StoredWord& w) {
  Chunk& c = chunks_[w.chunk];
  if (c.live > 0) c.live--;
  // The tail chunk is still accepting appends; keep it even when drained.
  if (c.live == 0 && c.data && w.chunk + 1 != chunkCount_) {
    c.data.reset();
  }
}
